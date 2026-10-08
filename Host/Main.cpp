#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "../Protocol.h"
#include <array>
#include <complex>
#include <thread>
#include <future>
#include <mutex>
#include "WindowWait.h"

using namespace juce;
namespace {
struct Configuration {
    PluginDescription desc;
    MemoryBlock state;
    std::array<String,pix::slots> ids;
    std::vector<std::pair<String,float>> baseValues;
    pix::ConfigHeader header;
    Configuration() = default;
    explicit Configuration(const std::vector<uint8_t>& b) {
        if (!pix::validConfig(b.data(),b.size())) throw std::runtime_error("Invalid PixSpectra configuration");
        std::memcpy(&header,b.data(),sizeof(header));
        if (!header.xmlBytes) return;
        auto xml=XmlDocument::parse(String::fromUTF8(reinterpret_cast<const char*>(b.data()+sizeof(header)),int(header.xmlBytes)));
        if (!xml || !xml->hasTagName("PixSpectra")) throw std::runtime_error("Invalid plugin state XML");
        auto* description=xml->getChildByName("PLUGIN");
        if (!description || !desc.loadFromXml(*description)) throw std::runtime_error("Invalid VST description");
        if (!state.fromBase64Encoding(xml->getStringAttribute("state"))) throw std::runtime_error("Invalid VST state");
        for (unsigned i=0;i<pix::slots;++i) ids[i]=xml->getStringAttribute("slot"+String(i));
        if(auto* base=xml->getChildByName("Parameters")) for(auto* item:base->getChildIterator()) {
            float value=float(item->getDoubleAttribute("value",-1));
            auto id=item->getStringAttribute("id");
            if(id.isEmpty()||!std::isfinite(value)||value<0||value>1) throw std::runtime_error("Invalid saved VST parameter");
            baseValues.emplace_back(id,value);
        }
    }
    std::vector<uint8_t> pack() {
        auto x=std::make_unique<XmlElement>("PixSpectra");
        x->addChildElement(desc.createXml().release()); x->setAttribute("state",state.toBase64Encoding());
        for (unsigned i=0;i<pix::slots;++i) x->setAttribute("slot"+String(i),ids[i]);
        auto* base=x->createNewChildElement("Parameters");
        for(const auto& value:baseValues) {auto* p=base->createNewChildElement("P");p->setAttribute("id",value.first);p->setAttribute("value",double(value.second));}
        auto s=x->toString(); header.xmlBytes=uint32_t(s.getNumBytesAsUTF8());
        if (sizeof(header)+header.xmlBytes>pix::maxConfig) throw std::runtime_error("VST state exceeds 16 MiB");
        std::vector<uint8_t> b(sizeof(header)+header.xmlBytes);
        std::memcpy(b.data(),&header,sizeof(header)); std::memcpy(b.data()+sizeof(header),s.toRawUTF8(),header.xmlBytes); return b;
    }
};
String parameterID(AudioProcessorParameter* p) {
    if (auto* h=dynamic_cast<HostedAudioProcessorParameter*>(p)) return h->getParameterID();
    throw std::runtime_error("VST parameter has no stable ID");
}
AudioProcessorParameter* findParam(AudioPluginInstance& p,const String& id) {
    if (id.isEmpty()) return nullptr;
    for (auto* a:p.getParameters()) if (parameterID(a)==id) return a;
    return nullptr;
}
String slotLabel(AudioProcessorParameter& p) {
    if(p.isDiscrete())return p.getName(128).substring(0,14)+" ["+String(p.getNumSteps())+" steps]";
    return p.getName(128).substring(0,24);
}
void captureState(AudioPluginInstance& p,Configuration& c) {
    // Keep every exposed value, including those with no AE slot. A non-running
    // VST processor may not yet have consumed changes from its native editor.
    c.baseValues.clear();
    for(auto* q:p.getParameters()) if(q->isAutomatable() && !q->isMetaParameter())
        c.baseValues.emplace_back(parameterID(q),q->getValue());
    p.getStateInformation(c.state);
}
std::unique_ptr<AudioPluginInstance> load(AudioPluginFormatManager& formats, const Configuration& c) {
    String error;
    auto p=formats.createPluginInstance(c.desc,48000,512,error);
    if (!p) throw std::runtime_error(error.toStdString());
    if (p->getTotalNumInputChannels()<1) throw std::runtime_error("Select an audio effect, not an instrument");
    p->disableNonMainBuses();
    AudioProcessor::BusesLayout layout=p->getBusesLayout();
    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty()) throw std::runtime_error("Audio buses are missing");
    layout.inputBuses.getReference(0)=AudioChannelSet::stereo();
    layout.outputBuses.getReference(0)=AudioChannelSet::stereo();
    if (!p->setBusesLayout(layout)) {
        layout.inputBuses.getReference(0)=AudioChannelSet::mono();
        layout.outputBuses.getReference(0)=AudioChannelSet::mono();
        if (!p->setBusesLayout(layout)) throw std::runtime_error("VST must support mono or stereo input/output");
    }
    if (c.state.getSize()) p->setStateInformation(c.state.getData(),int(c.state.getSize()));
    for(const auto& value:c.baseValues) {
        auto* q=findParam(*p,value.first);
        if(!q) throw std::runtime_error("Saved VST parameter is missing; plugin version may have changed");
        q->setValueNotifyingHost(value.second);
    }
    return p;
}
void applyValues(AudioPluginInstance& p,const Configuration& c,const float* values) {
    for (unsigned i=0;i<pix::slots;++i) if (c.ids[i].isNotEmpty()) {
        auto* q=findParam(p,c.ids[i]);
        if (!q) throw std::runtime_error("A mapped VST parameter is missing; reconfigure this VST");
        q->setValueNotifyingHost(values[i]);
    }
}
struct Playhead : AudioPlayHead {
    int64_t sample=0; double bpm=120;
    Optional<PositionInfo> getPosition() const override {
        PositionInfo p; p.setTimeInSamples(sample); p.setTimeInSeconds(double(sample)/48000);
        p.setBpm(bpm); p.setPpqPosition(double(sample)*bpm/(48000*60));
        p.setTimeSignature(TimeSignature{4,4}); p.setIsPlaying(true); return p;
    }
};

// Full complex FFT keeps exact bin packing explicit. Inverse is 1/N normalized by JUCE.
std::vector<float> render(AudioPluginFormatManager& formats, const Configuration& c,
                          const pix::Request& r,const std::vector<float>& rgb) {
    pix::validate(r);
    const size_t pixels=size_t(r.width)*r.height;
    if (rgb.size()!=pixels*3) throw std::runtime_error("Incorrect RGB buffer size");
    for (float v:rgb) if (!std::isfinite(v) || v<0) throw std::runtime_error("Non-finite or negative RGB input");
    int order=1; while ((1u<<order)<2*(r.height+1)) ++order;
    const int n=1<<order; const size_t length=size_t(n)*r.width;
    if (length>64*1024*1024) throw std::runtime_error("Spectral buffer too large; reduce layer resolution");
    std::vector<std::complex<float>> phase(r.height);
    // Same fixed phase for every colour/frame; equal RGB remains equal for deterministic DSP.
    for (uint32_t y=0;y<r.height;++y) {
        uint32_t h=(y+1)*2654435761u; h^=h>>16; h*=2246822519u; h^=h>>13;
        phase[y]=std::polar(1.0f,float(double(h)/4294967296.0*6.283185307179586));
    }
    const float gain=std::pow(10.0f,r.inputDb/20.0f);
    std::vector<float> output(rgb.size());
    // Construct/controllers stay on the JUCE message thread; independent VST
    // processors and FFT buffers run on separate audio workers, as in a DAW.
    std::array<std::unique_ptr<AudioPluginInstance>,3> processors;
    std::array<Playhead,3> playheads;
    struct ProcessorCleanup {
        decltype(processors)& p;
        ~ProcessorCleanup(){for(auto& item:p)if(item){item->releaseResources();item->setPlayHead(nullptr);}}
    } cleanup{processors};
    if(c.header.xmlBytes) for(unsigned i=0;i<3;++i) {
        auto& p=processors[i];p=load(formats,c);playheads[i].bpm=r.bpm;
        p->setPlayHead(&playheads[i]);p->setNonRealtime(true);applyValues(*p,c,r.values);
        p->prepareToPlay(48000,512);p->reset();applyValues(*p,c,r.values);
    }
    std::array<std::future<void>,3> jobs;
    for (unsigned colour=0;colour<3;++colour) jobs[colour]=std::async(std::launch::async,[&,colour] {
        dsp::FFT fft(order);
        std::vector<std::complex<float>> spectrum(n),wave(n),analysed(n);
        std::vector<float> pcm(length);
        for (unsigned x=0;x<r.width;++x) {
            std::fill(spectrum.begin(),spectrum.end(),std::complex<float>{});
            for (unsigned y=0;y<r.height;++y) {
                const unsigned bin=r.height-y;
                spectrum[bin]=phase[y]*rgb[(size_t(y)*r.width+x)*3+colour];
                spectrum[n-bin]=std::conj(spectrum[bin]);
            }
            fft.perform(spectrum.data(),wave.data(),true);
            for (int k=0;k<n;++k) pcm[size_t(x)*n+k]=wave[k].real()*gain;
        }
        if (c.header.xmlBytes) {
            auto* p=processors[colour].get(); auto& play=playheads[colour];
            const int latency=p->getLatencySamples();
            if (latency<0 || latency>480000) throw std::runtime_error("Unsupported VST latency (over 10 seconds)");
            // Fixed warm-up, then latency-flush. Tail outside the image is deliberately cropped.
            const int warm=12288;
            const size_t total=warm+length+size_t(latency);
            std::vector<float> result(length,0);
            const int channels=std::max(p->getTotalNumInputChannels(),p->getTotalNumOutputChannels());
            AudioBuffer<float> block(channels,512); MidiBuffer midi;
            for (size_t pos=0;pos<total;pos+=512) {
                const int count=int(std::min(size_t(512),total-pos));
                block.setSize(channels,count,false,false,true); block.clear(); midi.clear();
                for (int k=0;k<count;++k) {
                    const auto at=pos+k;
                    float v=at>=warm && at-warm<length?pcm[at-warm]:0;
                    for (int ch=0;ch<p->getTotalNumInputChannels();++ch) block.setSample(ch,k,v);
                }
                play.sample=int64_t(pos)-warm;
                p->processBlock(block,midi);
                if (p->getLatencySamples()!=latency) throw std::runtime_error("VST changed latency during rendering");
                for (int k=0;k<count;++k) {
                    const auto at=pos+k;
                    if (at<size_t(warm+latency) || at-size_t(warm+latency)>=length) continue;
                    float v=block.getSample(0,k);
                    if (p->getTotalNumOutputChannels()>1) v=0.5f*(v+block.getSample(1,k));
                    if (!std::isfinite(v)) throw std::runtime_error("VST produced a non-finite sample");
                    result[at-size_t(warm+latency)]=v;
                }
            }
            pcm.swap(result);
        }
        for (unsigned x=0;x<r.width;++x) {
            for(int k=0;k<n;++k) wave[k]={pcm[size_t(x)*n+k],0};
            fft.perform(wave.data(),analysed.data(),false);
            for (unsigned y=0;y<r.height;++y) {
                float v=std::abs(analysed[r.height-y])/gain;
                if(!std::isfinite(v)) throw std::runtime_error("Non-finite decoded image");
                output[(size_t(y)*r.width+x)*3+colour]=v;
            }
        }
    });
    // Join all workers before destroying processors, also when one job fails.
    std::exception_ptr failure;
    for(auto& job:jobs) {try{job.get();}catch(...){if(!failure)failure=std::current_exception();}}
    if(failure)std::rethrow_exception(failure);
    return output;
}

struct EditorWindow : DocumentWindow {
    EditorWindow(AudioPluginInstance& p):DocumentWindow(p.getName(),Colour(0xff252525),closeButton) {
        setUsingNativeTitleBar(true);
        auto* editor=p.hasEditor()?p.createEditorIfNeeded():nullptr;
        if (!editor) editor=new GenericAudioProcessorEditor(p);
        setContentOwned(editor,true); centreWithSize(getWidth(),getHeight()); setVisible(true);
    }
    void closeButtonPressed() override { setVisible(false); }
};
struct Setup : Component, private Timer {
    AudioPluginFormatManager& formats; Configuration c;
    std::function<void(int)> finish;
    std::vector<uint8_t> result;
    std::unique_ptr<AudioPluginInstance> plugin;
    std::unique_ptr<EditorWindow> editor;
    std::unique_ptr<FileChooser> chooser;
    Label title, status; ComboBox plugins;
    TextButton browse{"Browse..."}, open{"Open VST editor"}, apply{"Apply"}, cancel{"Cancel"};
    std::array<Label,pix::slots> labels;
    std::array<ComboBox,pix::slots> mappings;
    std::array<Slider,pix::slots> values;
    Viewport viewport;
    Component rows;
    StringArray paths;
    std::vector<std::pair<File,int>> scanDirectories;
    bool refreshing=false;
    Setup(AudioPluginFormatManager& f,Configuration initial):formats(f),c(std::move(initial)) {
        setSize(800,700);
        addAndMakeVisible(viewport);viewport.setViewedComponent(&rows,false);
        viewport.setScrollBarsShown(true,false);
        title.setText("VST effect and AE automation",dontSendNotification);
        title.setFont(FontOptions(20));
        for (Component* w:std::initializer_list<Component*>{&title,&status,&plugins,&browse,&open,&apply,&cancel}) addAndMakeVisible(w);
        // Display the selector before filesystem discovery. Scan one directory
        // per timer tick; never load a VST simply to populate the selector.
        for (auto root:{File("C:\\Program Files\\Common Files\\VST3"),File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("Programs/Common/VST3")})
            scanDirectories.emplace_back(root,0);
        plugins.setTextWhenNothingSelected("Choose a VST3 effect");
        plugins.onChange=[this]{ if(!refreshing && plugins.getSelectedId()>0) selectPath(paths[plugins.getSelectedId()-1]); };
        browse.onClick=[this]{ chooser=std::make_unique<FileChooser>("Choose VST3 file or bundle",File("C:\\Program Files\\Common Files\\VST3"),"*.vst3");
            chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectFiles|FileBrowserComponent::canSelectDirectories,
            [this](const FileChooser& fc){ auto file=fc.getResult(); if(file.exists()) selectPath(file.getFullPathName()); }); };
        open.onClick=[this]{ if(plugin) { if(!editor) editor=std::make_unique<EditorWindow>(*plugin); editor->setVisible(true); editor->toFront(true); } };
        apply.onClick=[this]{ save(); }; cancel.onClick=[this]{ if(finish) finish(2); };
        for(unsigned i=0;i<pix::slots;++i) {
            labels[i].setText("AE "+String(i+1),dontSendNotification);
            rows.addAndMakeVisible(labels[i]); rows.addAndMakeVisible(mappings[i]); rows.addAndMakeVisible(values[i]);
            values[i].setRange(0,100,0); values[i].setSliderStyle(Slider::LinearHorizontal);
            values[i].setTextBoxStyle(Slider::TextBoxRight,false,100,24);
            values[i].textFromValueFunction=[this,i](double v){
                auto* p=plugin?findParam(*plugin,c.ids[i]):nullptr;
                return p?p->getText(float(v/100),128):String(v,2)+" %";
            };
            values[i].valueFromTextFunction=[this,i](const String& text){
                auto* p=plugin?findParam(*plugin,c.ids[i]):nullptr;
                return p?double(p->getValueForText(text))*100:text.getDoubleValue();
            };
            mappings[i].onChange=[this,i]{ if(refreshing || !plugin) return;
                int index=mappings[i].getSelectedId()-2;
                c.ids[i]=index>=0?parameterID(plugin->getParameters()[index]):String{};
                sync(); values[i].updateText();resized(); };
            values[i].onValueChange=[this,i]{ if(!refreshing && plugin) if(auto* p=findParam(*plugin,c.ids[i])) p->setValueNotifyingHost(float(values[i].getValue()/100)); };
        }
        if(c.header.xmlBytes) { try { plugin=load(formats,c); applyValues(*plugin,c,c.header.values); populate(); } catch(const std::exception& e){status.setText(e.what(),dontSendNotification);} }
        else populate();
        startTimerHz(10);
    }
    ~Setup() override { stopTimer(); editor.reset(); plugin.reset(); }
    void discover(const File& root,int depth) {
        if(depth>6 || !root.isDirectory()) return;
        for(const auto& f:root.findChildFiles(File::findFilesAndDirectories,false)) {
            if(f.hasFileExtension("vst3")) paths.addIfNotAlreadyThere(f.getFullPathName());
            else if(f.isDirectory() && !f.isSymbolicLink()) scanDirectories.emplace_back(f,depth+1);
        }
    }
    void selectPath(const String& path) {
        try {
            OwnedArray<PluginDescription> found;
            formats.getFormat(0)->findAllTypesForFile(found,path);
            if(found.isEmpty()) throw std::runtime_error("No compatible VST3 effect found");
            if(found.size()>1) throw std::runtime_error("Multi-effect VST3 bundles are not yet supported; select a single-effect VST3");
            Configuration next; next.desc=*found[0]; auto p=load(formats,next);
            editor.reset(); plugin=std::move(p); c=std::move(next);
            int s=0; for(auto* q:plugin->getParameters()) if(s<int(pix::slots) && q->isAutomatable() && !q->isMetaParameter()) c.ids[s++]=parameterID(q);
            populate();
        } catch(const std::exception& e){status.setText(e.what(),dontSendNotification);}
    }
    void populate() {
        refreshing=true;
        for(unsigned i=0;i<pix::slots;++i) {
            mappings[i].clear(dontSendNotification); mappings[i].addItem("Unassigned",1);
            if(plugin) { int k=2; for(auto* q:plugin->getParameters()) {
                mappings[i].addItem(q->getName(128),k);
                mappings[i].setItemEnabled(k,q->isAutomatable() && !q->isMetaParameter()); ++k;
            } }
            int id=1;
            if(plugin) for(int k=0;k<plugin->getParameters().size();++k) if(parameterID(plugin->getParameters()[k])==c.ids[i]) id=k+2;
            mappings[i].setSelectedId(id,dontSendNotification);
            values[i].updateText();
        }
        if(plugin) {
            const int index=paths.indexOf(c.desc.fileOrIdentifier);
            plugins.setSelectedId(index>=0?index+1:0,dontSendNotification);
            if(index<0) plugins.setText(plugin->getName(),dontSendNotification);
        }
        refreshing=false;
        open.setEnabled(bool(plugin)); apply.setEnabled(bool(plugin));
        if(plugin) status.setText(plugin->getName()+" | Native values shown here; AE uses normalized 0-100. Slot keyframes are retained.",dontSendNotification);
        sync();
        resized();
    }
    void sync() {
        refreshing=true;
        for(unsigned i=0;i<pix::slots;++i) {
            auto* q=plugin?findParam(*plugin,c.ids[i]):nullptr;
            values[i].setEnabled(q!=nullptr);
            if(q && !values[i].isMouseButtonDown()) values[i].setValue(q->getValue()*100,dontSendNotification);
        }
        refreshing=false;
    }
    void timerCallback() override {
        if(!scanDirectories.empty()) {
            auto next=scanDirectories.back();scanDirectories.pop_back();
            const int before=paths.size();discover(next.first,next.second);
            for(int i=before;i<paths.size();++i) plugins.addItem(File(paths[i]).getFileNameWithoutExtension(),i+1);
        }
        sync();
    }
    void save() {
        try {
            if(!plugin) return;
            for(unsigned i=0;i<pix::slots;++i) for(unsigned j=0;j<i;++j)
                if(c.ids[i].isNotEmpty() && c.ids[i]==c.ids[j]) throw std::runtime_error("Each VST parameter may be assigned to only one AE slot");
            c.header=pix::ConfigHeader{};
            plugin->getName().copyToUTF8(c.header.plugin,sizeof(c.header.plugin));
            for(unsigned i=0;i<pix::slots;++i) if(auto* p=findParam(*plugin,c.ids[i])) {
                slotLabel(*p).copyToUTF8(c.header.labels[i],32); c.header.values[i]=p->getValue();
            }
            captureState(*plugin,c); result=c.pack(); if(finish) finish(0);
        } catch(const std::exception& e){ status.setText(e.what(),dontSendNotification); }
    }
    void paint(Graphics& g) override { g.fillAll(Colour(0xff252525)); }
    void resized() override {
        auto r=getLocalBounds().reduced(20); title.setBounds(r.removeFromTop(32));
        r.removeFromTop(10); auto row=r.removeFromTop(30); browse.setBounds(row.removeFromRight(100)); row.removeFromRight(8); plugins.setBounds(row);
        r.removeFromTop(8); open.setBounds(r.removeFromTop(28).removeFromLeft(160));
        r.removeFromTop(6); status.setBounds(r.removeFromTop(34)); r.removeFromTop(8);
        r.removeFromBottom(40);viewport.setBounds(r);
        unsigned count=1;for(unsigned i=0;i<pix::slots;++i)if(c.ids[i].isNotEmpty())count=std::min(pix::slots,i+2);
        rows.setSize(r.getWidth()-viewport.getScrollBarThickness(),int(count)*29);
        auto content=rows.getLocalBounds();
        for(unsigned i=0;i<pix::slots;++i) {bool show=i<count;labels[i].setVisible(show);mappings[i].setVisible(show);values[i].setVisible(show);
            auto line=content.removeFromTop(29); labels[i].setBounds(line.removeFromLeft(55));
            mappings[i].setBounds(line.removeFromLeft(330).reduced(0,2)); line.removeFromLeft(12); values[i].setBounds(line); }
        auto footer=getLocalBounds().reduced(20).removeFromBottom(30);
        apply.setBounds(footer.removeFromRight(100)); footer.removeFromRight(8); cancel.setBounds(footer.removeFromRight(100));
    }
};
struct MainWindow : DocumentWindow {
    MainWindow(Component* c,bool show=true):DocumentWindow("PixSpectra — VST setup",Colour(0xff252525),closeButton) {
        setUsingNativeTitleBar(true); setContentOwned(c,true); centreWithSize(getWidth(),getHeight()); setVisible(show);
    }
    void closeButtonPressed() override { exitModalState(2); }
};
// The engine owns one JUCE message thread in the AE process. AE UI/render callers
// serialize here; VST controller and processor never race or share render state.
struct Service {
    std::mutex serial;
    std::thread worker;
    std::unique_ptr<AudioPluginFormatManager> formats;
    pix::Request cachedRequest{};
    std::vector<uint8_t> cachedConfig;
    std::vector<float> cachedInput,cachedOutput;
    bool cached=false;
    void start() {
        if(worker.joinable()) return;
        std::promise<void> ready; auto future=ready.get_future();
        worker=std::thread([this,&ready]{
            bool initialized=false;
            try {
                ScopedJuceInitialiser_GUI gui;
                formats=std::make_unique<AudioPluginFormatManager>();
                formats->addFormat(std::make_unique<VST3PluginFormat>());
                initialized=true; ready.set_value();
                MessageManager::getInstance()->runDispatchLoop();
                formats.reset();
            } catch(...) { formats.reset(); if(!initialized) ready.set_exception(std::current_exception()); }
        });
        try {pix::waitForWindowFuture(future);} catch(...) {worker.join();throw;}
    }
    int run(std::function<int()> operation,char* error,uint32_t capacity,bool modal=false) {
        // Dispatching host messages may re-enter an effect callback. Never
        // reacquire the serial mutex from that same caller's nested callback.
        static thread_local bool active=false;
        if(active) {if(error && capacity)String("PixSpectra operation already in progress").copyToUTF8(error,capacity);return 1;}
        struct Guard {bool& flag;Guard(bool& f):flag(f){flag=true;}~Guard(){flag=false;}} guard{active};
        std::lock_guard<std::mutex> lock(serial);
        if(error && capacity) error[0]=0;
        try {
            start(); std::promise<int> done; auto future=done.get_future();
            if(!MessageManager::callAsync([&]{
                try { done.set_value(operation()); }
                catch(...) { done.set_exception(std::current_exception()); }
            })) throw std::runtime_error("Cannot dispatch VST operation");
            return pix::waitForWindowFuture(future,modal);
        } catch(const std::exception& e) {
            if(error && capacity) String(e.what()).copyToUTF8(error,capacity); return 1;
        } catch(...) {
            if(error && capacity) String("Unknown VST engine error").copyToUTF8(error,capacity); return 1;
        }
    }
    void stop() {
        std::lock_guard<std::mutex> lock(serial);
        if(worker.joinable()) {
            MessageManager::callAsync([]{MessageManager::getInstance()->stopDispatchLoop();});
            auto joined=std::async(std::launch::async,[this]{worker.join();});
            pix::waitForWindowFuture(joined);
        }
        cached=false;cachedConfig.clear();cachedInput.clear();cachedOutput.clear();
    }
};
Service& service() { static auto* s=new Service; return *s; }
void allocateResult(const std::vector<uint8_t>& data,uint8_t** out,uint32_t* size) {
    auto* p=static_cast<uint8_t*>(std::malloc(data.size())); if(!p) throw std::bad_alloc();
    std::memcpy(p,data.data(),data.size()); *out=p; *size=uint32_t(data.size());
}
}
#define PIX_EXPORT extern "C" __declspec(dllexport)
// Regression probe: construct the real selector and native peer without showing
// a window, selecting a VST, or calling any Adobe API.
PIX_EXPORT int PixCheckSelector(char* error,uint32_t cap) {
    return service().run([&]{
        auto* setup=new Setup(*service().formats,Configuration{});
        MainWindow window(setup,false);
        auto* peer=window.getPeer();
        if(!peer || peer->getCurrentRenderingEngine()!=0)
            throw std::runtime_error("Selector did not use the software window renderer");
        Image image(Image::ARGB,800,700,true);
        if(image.getPixelData()->createType()->getTypeID()!=SoftwareImageType().getTypeID())
            throw std::runtime_error("Selector image unexpectedly uses a GPU backend");
        Graphics graphics(image);setup->paintEntireComponent(graphics,false);
        return 0;
    },error,cap);
}
PIX_EXPORT int PixConfigureModal(const uint8_t* data,uint32_t size,uint8_t** out,uint32_t* outSize,char* error,uint32_t cap,void* owner) {
    if(!out || !outSize) return 1; *out=nullptr; *outSize=0;
    pix::ModalOwner modalOwner(owner);
    return service().run([&]{
        if(!pix::validConfig(data,size)) throw std::runtime_error("Invalid setup state");
        service().cached=false;
        auto* setup=new Setup(*service().formats,Configuration({data,data+size}));
        MainWindow window(setup); setup->finish=[&window](int code){window.exitModalState(code);};
        const int code=window.runModalLoop();
        if(code==0) allocateResult(setup->result,out,outSize); return code;
    },error,cap,true);
}
// Retained for standalone callers from the first engine ABI.
PIX_EXPORT int PixConfigure(const uint8_t* data,uint32_t size,uint8_t** out,uint32_t* outSize,char* error,uint32_t cap) {
    return PixConfigureModal(data,size,out,outSize,error,cap,nullptr);
}
PIX_EXPORT int PixRender(const pix::Request* request,const uint8_t* config,const float* rgb,float* output,char* error,uint32_t cap) {
    return service().run([&]{
        if(!request || !config || !rgb || !output) throw std::runtime_error("Null render argument");
        pix::validate(*request);
        if(!pix::validConfig(config,request->configBytes)) throw std::runtime_error("Invalid VST configuration");
        auto& s=service();const size_t count=size_t(request->width)*request->height*3;
        if(s.cached && !std::memcmp(request,&s.cachedRequest,sizeof(*request)) &&
           s.cachedConfig.size()==request->configBytes && !std::memcmp(config,s.cachedConfig.data(),request->configBytes) &&
           s.cachedInput.size()==count && !std::memcmp(rgb,s.cachedInput.data(),count*sizeof(float))) {
            std::memcpy(output,s.cachedOutput.data(),count*sizeof(float));return 0;
        }
        s.cached=false;
        auto result=render(*service().formats,Configuration({config,config+request->configBytes}),*request,
            {rgb,rgb+size_t(request->width)*request->height*3});
        std::memcpy(output,result.data(),result.size()*sizeof(float));
        // Exact-key, one-frame cache (maximum 128 MiB). Mix/alpha are applied by
        // the AEX afterwards. A changed pixel, VST state or automation invalidates it.
        if(count*sizeof(float)*2+request->configBytes<=128*1024*1024) {
            s.cachedRequest=*request;s.cachedConfig.assign(config,config+request->configBytes);
            s.cachedInput.assign(rgb,rgb+count);s.cachedOutput=std::move(result);s.cached=true;
        } else {s.cachedConfig.clear();s.cachedInput.clear();s.cachedOutput.clear();}
        return 0;
    },error,cap);
}
// Test utility: capture an installed effect through exactly the production loader.
PIX_EXPORT int PixCapture(const wchar_t* path,uint8_t** out,uint32_t* outSize,char* error,uint32_t cap) {
    if(!out || !outSize) return 1; *out=nullptr; *outSize=0;
    return service().run([&]{
        OwnedArray<PluginDescription> found; service().formats->getFormat(0)->findAllTypesForFile(found,String(path));
        if(found.size()!=1) throw std::runtime_error("Expected one VST3 effect");
        Configuration c; c.desc=*found[0]; auto p=load(*service().formats,c); captureState(*p,c);
        p->getName().copyToUTF8(c.header.plugin,sizeof(c.header.plugin));
        unsigned i=0; for(auto* q:p->getParameters()) if(i<pix::slots && q->isAutomatable()) {
            c.ids[i]=parameterID(q); c.header.values[i]=q->getValue(); slotLabel(*q).copyToUTF8(c.header.labels[i],32); ++i;
        }
        allocateResult(c.pack(),out,outSize); return 0;
    },error,cap);
}
PIX_EXPORT void PixFree(void* p) { std::free(p); }
// Read-only parameter audit using the production loader, without opening an editor.
PIX_EXPORT int PixDescribe(const uint8_t* data,uint32_t size,uint8_t** out,uint32_t* outSize,char* error,uint32_t cap) {
    if(!out || !outSize)return 1;*out=nullptr;*outSize=0;
    return service().run([&]{
        Configuration c({data,data+size});auto p=load(*service().formats,c);
        Array<var> parameters;
        for(auto* q:p->getParameters()) {
            if(!q->isAutomatable() || q->isMetaParameter())continue;
            auto* item=new DynamicObject;
            item->setProperty("id",parameterID(q));item->setProperty("name",q->getName(128));
            item->setProperty("discrete",q->isDiscrete());item->setProperty("steps",q->getNumSteps());
            item->setProperty("unit",q->getLabel());item->setProperty("value",q->getValue());
            item->setProperty("bypass",q==p->getBypassParameter());
            Array<var> samples;
            for(float v:{0.0f,0.25f,0.49f,0.5f,0.51f,0.75f,1.0f}) {
                auto* sample=new DynamicObject;q->setValueNotifyingHost(v);
                sample->setProperty("sent",v);sample->setProperty("received",q->getValue());
                sample->setProperty("text",q->getText(v,128));samples.add(var(sample));
            }
            item->setProperty("samples",samples);parameters.add(var(item));
        }
        auto json=JSON::toString(var(parameters));auto* begin=reinterpret_cast<const uint8_t*>(json.toRawUTF8());
        allocateResult({begin,begin+json.getNumBytesAsUTF8()},out,outSize);return 0;
    },error,cap);
}
PIX_EXPORT void PixShutdown() { service().stop(); }
