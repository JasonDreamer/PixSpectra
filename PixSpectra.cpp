#include "PixSpectra.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <memory>
#include <cstdio>

namespace {
std::mutex engineMutex;
AEGP_PluginID pluginId=0;
bool registered=false;
HMODULE engineModule=nullptr;
PixConfigure configureFn=nullptr;
PixRender renderFn=nullptr;
PixFree freeFn=nullptr;
PixShutdown shutdownFn=nullptr;

void loadEngine() {
    if(engineModule) return;
    HMODULE self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&EffectMain),&self)) throw std::runtime_error("Cannot locate PixSpectra");
    wchar_t path[32768]; auto n=GetModuleFileNameW(self,path,32768);
    if(!n || n>=32768) throw std::runtime_error("Cannot locate PixSpectra folder");
    std::wstring directory(path,n); directory.resize(directory.find_last_of(L"\\/")+1);
    HMODULE dll=LoadLibraryExW((directory+L"PixSpectraEngine.dll").c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!dll) throw std::runtime_error("PixSpectraEngine.dll is missing or could not be loaded");
    auto c=reinterpret_cast<PixConfigure>(GetProcAddress(dll,"PixConfigureModal"));
    auto r=reinterpret_cast<PixRender>(GetProcAddress(dll,"PixRender"));
    auto f=reinterpret_cast<PixFree>(GetProcAddress(dll,"PixFree"));
    auto s=reinterpret_cast<PixShutdown>(GetProcAddress(dll,"PixShutdown"));
    if(!c||!r||!f||!s) {FreeLibrary(dll); throw std::runtime_error("Incompatible PixSpectraEngine.dll");}
    engineModule=dll; configureFn=c; renderFn=r; freeFn=f; shutdownFn=s;
}
void message(PF_OutData* out_data,const char* text) {
    if(out_data) { strncpy_s(out_data->return_msg,text,_TRUNCATE); out_data->out_flags|=PF_OutFlag_DISPLAY_ERROR_MESSAGE; }
}
std::vector<uint8_t> readConfig(PF_InData* in_data,PF_Handle h) {
    if(!h) return pix::emptyConfig();
    auto size=PF_GET_HANDLE_SIZE(h);
    if(size>pix::maxConfig || size<12) throw std::runtime_error("Invalid VST state handle");
    std::vector<uint8_t> b(static_cast<size_t>(size));
    const auto* p=PF_LOCK_HANDLE(h); if(!p) throw std::bad_alloc();
    std::memcpy(b.data(),p,size_t(size)); PF_UNLOCK_HANDLE(h);
    return pix::upgradeConfig(b.data(),b.size());
}
PF_Handle makeHandle(PF_InData* in_data,const std::vector<uint8_t>& b) {
    PF_Handle h=PF_NEW_HANDLE(b.size()); if(!h) throw std::bad_alloc();
    void* p=PF_LOCK_HANDLE(h); if(!p){ PF_DISPOSE_HANDLE(h); throw std::bad_alloc(); }
    std::memcpy(p,b.data(),b.size()); PF_UNLOCK_HANDLE(h); return h;
}
PF_Err arbitrary(PF_InData* in_data,PF_ArbParamsExtra* ex) {
    if(!ex || ex->id!=CONFIG_ID) return PF_Err_BAD_CALLBACK_PARAM;
    auto& u=ex->u;
    switch(ex->which_function) {
    case PF_Arbitrary_NEW_FUNC: *u.new_func_params.arbPH=makeHandle(in_data,pix::emptyConfig()); break;
    case PF_Arbitrary_DISPOSE_FUNC: if(u.dispose_func_params.arbH) PF_DISPOSE_HANDLE(u.dispose_func_params.arbH); break;
    case PF_Arbitrary_COPY_FUNC: *u.copy_func_params.dst_arbPH=makeHandle(in_data,readConfig(in_data,u.copy_func_params.src_arbH)); break;
    case PF_Arbitrary_FLAT_SIZE_FUNC: *u.flat_size_func_params.flat_data_sizePLu=A_u_long(readConfig(in_data,u.flat_size_func_params.arbH).size()); break;
    case PF_Arbitrary_FLATTEN_FUNC: {
        auto b=readConfig(in_data,u.flatten_func_params.arbH);
        if(u.flatten_func_params.buf_sizeLu<b.size()) return PF_Err_BAD_CALLBACK_PARAM;
        std::memcpy(u.flatten_func_params.flat_dataPV,b.data(),b.size()); break;
    }
    case PF_Arbitrary_UNFLATTEN_FUNC: {
        const auto* p=static_cast<const uint8_t*>(u.unflatten_func_params.flat_dataPV); auto n=u.unflatten_func_params.buf_sizeLu;
        *u.unflatten_func_params.arbPH=makeHandle(in_data,pix::upgradeConfig(p,n)); break;
    }
    case PF_Arbitrary_INTERP_FUNC:
        *u.interp_func_params.interpPH=makeHandle(in_data,readConfig(in_data,u.interp_func_params.tF<1?u.interp_func_params.left_arbH:u.interp_func_params.right_arbH)); break;
    case PF_Arbitrary_COMPARE_FUNC:
        *u.compare_func_params.compareP=readConfig(in_data,u.compare_func_params.a_arbH)==readConfig(in_data,u.compare_func_params.b_arbH)?PF_ArbCompare_EQUAL:PF_ArbCompare_NOT_EQUAL; break;
    // Text transport uses reversible hex, so copy/paste of arbitrary values is valid.
    case PF_Arbitrary_PRINT_SIZE_FUNC: *u.print_size_func_params.print_sizePLu=A_u_long(readConfig(in_data,u.print_size_func_params.arbH).size()*2+1); break;
    case PF_Arbitrary_PRINT_FUNC: {
        auto b=readConfig(in_data,u.print_func_params.arbH);
        if(u.print_func_params.print_sizeLu<b.size()*2+1) return PF_Err_BAD_CALLBACK_PARAM;
        const char* hex="0123456789abcdef"; auto* dst=u.print_func_params.print_bufferPC;
        for(size_t i=0;i<b.size();++i) {dst[2*i]=hex[b[i]>>4];dst[2*i+1]=hex[b[i]&15];} dst[b.size()*2]=0; break;
    }
    case PF_Arbitrary_SCAN_FUNC: {
        auto n=u.scan_func_params.bytes_to_scanLu; auto* p=u.scan_func_params.bufPC;
        if(n && p[n-1]==0) --n;
        if(n%2 || n>pix::maxConfig*2) return PF_Err_CANNOT_PARSE_KEYFRAME_TEXT;
        std::vector<uint8_t> b(n/2);
        auto digit=[](char ch){if(ch>='0'&&ch<='9')return ch-'0'; if(ch>='a'&&ch<='f')return ch-'a'+10;return -1;};
        for(size_t i=0;i<b.size();++i) { int a=digit(p[2*i]),v=digit(p[2*i+1]); if(a<0||v<0)return PF_Err_CANNOT_PARSE_KEYFRAME_TEXT; b[i]=uint8_t(a*16+v); }
        *u.scan_func_params.arbPH=makeHandle(in_data,pix::upgradeConfig(b.data(),b.size())); break;
    }
    default: return PF_Err_BAD_CALLBACK_PARAM;
    }
    return PF_Err_NONE;
}
PF_Err paramsSetup(PF_InData* in_data,PF_OutData* out_data) {
    PF_ParamDef def{};
    // Keep the copied template's two parameter indices and Disk IDs reserved.
    def.ui_flags=PF_PUI_INVISIBLE;
    PF_ADD_FLOAT_SLIDERX("Legacy Gain",0,100,0,100,10,PF_Precision_HUNDREDTHS,0,0,1);
    AEFX_CLR_STRUCT(def); def.ui_flags=PF_PUI_INVISIBLE;
    PF_ADD_COLOR("Legacy Color",128,255,255,2);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ARBITRARY2("VST State",0,0,PF_ParamFlag_CANNOT_TIME_VARY,PF_PUI_INVISIBLE|PF_PUI_NO_ECW_UI,nullptr,CONFIG_ID,nullptr);
    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON("VST Effect","Choose / Edit...",0,PF_ParamFlag_SUPERVISE,SETUP_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Mix",0,100,0,100,100,PF_Precision_HUNDREDTHS,0,0,MIX_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Audio Input Gain (dB)",-60,60,-24,24,0,PF_Precision_HUNDREDTHS,0,0,INPUT_GAIN_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Audio Tempo (BPM)",20,400,40,240,120,PF_Precision_HUNDREDTHS,0,0,TEMPO_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("CUDA Pixel Transfer","Enabled",TRUE,0,GPU_ID);
    for(unsigned i=0;i<pix::slots;++i) {
        AEFX_CLR_STRUCT(def); char name[32]; sprintf_s(name,"%02u: Unassigned",i+1);
        PF_ADD_FLOAT_SLIDERX(name,0,100,0,100,0,PF_Precision_HUNDREDTHS,0,0,SLOT_ID+i);
    }
    out_data->num_params=NUM_PARAMS; return PF_Err_NONE;
}
PF_Err updateUI(PF_InData* in_data,PF_ParamDef* params[]) {
    auto b=readConfig(in_data,params[CONFIG]->u.arb_d.value); pix::ConfigHeader h;std::memcpy(&h,b.data(),sizeof(h));
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    if(!registered) {
        auto e=suites.UtilitySuite6()->AEGP_RegisterWithAEGP(nullptr,"PixSpectra",&pluginId);
        if(e) return e;
        registered=true;
    }
    AEGP_EffectRefH effect=nullptr;
    auto e=suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(pluginId,in_data->effect_ref,&effect);
    if(e) return e;
    // AE visibility is controlled by dynamic stream flags, not PF_PUI_INVISIBLE.
    struct DisposeEffect { AEGP_SuiteHandler& s; AEGP_EffectRefH h; ~DisposeEffect(){s.EffectSuite4()->AEGP_DisposeEffect(h);} } owner{suites,effect};
    for(unsigned i=0;i<pix::slots;++i) {
        auto def=*params[SLOT_FIRST+i];
        sprintf_s(def.PF_DEF_NAME,"%02u: %.26s",i+1,h.labels[i][0]?h.labels[i]:"Unassigned");
        if(!h.labels[i][0]) def.ui_flags|=PF_PUI_DISABLED; else def.ui_flags&=~PF_PUI_DISABLED;
        PF_Err e=suites.ParamUtilsSuite3()->PF_UpdateParamUI(in_data->effect_ref,SLOT_FIRST+i,&def); if(e)return e;
        AEGP_StreamRefH stream=nullptr;
        e=suites.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(pluginId,effect,SLOT_FIRST+i,&stream);if(e)return e;
        AEGP_DynStreamFlags flags=0;
        e=suites.DynamicStreamSuite4()->AEGP_GetDynamicStreamFlags(stream,&flags);
        bool hide=!h.labels[i][0];
        if(!e && bool(flags&AEGP_DynStreamFlag_HIDDEN)!=hide)
            e=suites.DynamicStreamSuite4()->AEGP_SetDynamicStreamFlag(stream,AEGP_DynStreamFlag_HIDDEN,FALSE,hide);
        suites.StreamSuite5()->AEGP_DisposeStream(stream);if(e)return e;
    }
    return PF_Err_NONE;
}
PF_Err configure(PF_InData* in_data,PF_OutData* out_data,PF_ParamDef* params[]) {
    auto b=readConfig(in_data,params[CONFIG]->u.arb_d.value);
    pix::ConfigHeader h;std::memcpy(&h,b.data(),sizeof(h));
    for(unsigned i=0;i<pix::slots;++i) h.values[i]=float(params[SLOT_FIRST+i]->u.fs_d.value/100);
    std::memcpy(b.data(),&h,sizeof(h));
    std::unique_lock<std::mutex> guard(engineMutex,std::try_to_lock);
    if(!guard.owns_lock()) return PF_Interrupt_CANCEL;
    loadEngine();
    void* owner=nullptr;AEGP_SuiteHandler suites(in_data->pica_basicP);
    auto ownerError=suites.UtilitySuite6()->AEGP_GetMainHWND(&owner);if(ownerError)return ownerError;
    uint8_t* result=nullptr; uint32_t size=0; char error[240]{};
    int code=configureFn(b.data(),uint32_t(b.size()),&result,&size,error,sizeof(error),owner);
    std::unique_ptr<uint8_t,PixFree> owned(result,freeFn);
    if(code==2) return PF_Err_NONE;
    if(code || !pix::validConfig(result,size)) {message(out_data,error[0]?error:"VST setup failed");return PF_Err_NONE;}
    PF_Handle replacement=makeHandle(in_data,{result,result+size});
    auto old=params[CONFIG]->u.arb_d.value; params[CONFIG]->u.arb_d.value=replacement;
    if(old) PF_DISPOSE_HANDLE(old);
    params[CONFIG]->uu.change_flags=PF_ChangeFlag_CHANGED_VALUE;
    std::memcpy(&h,result,sizeof(h));
    for(unsigned i=0;i<pix::slots;++i) {
        params[SLOT_FIRST+i]->u.fs_d.value=h.values[i]*100;
        params[SLOT_FIRST+i]->uu.change_flags=PF_ChangeFlag_CHANGED_VALUE;
    }
    out_data->out_flags|=PF_OutFlag_FORCE_RERENDER|PF_OutFlag_REFRESH_UI;
    return updateUI(in_data,params);
}
template<class Pixel> void packCPU(const PF_LayerDef& input,std::vector<float>& rgb,float scale) {
    for(int y=0;y<input.height;++y) {
        const auto* row=reinterpret_cast<const Pixel*>(reinterpret_cast<const char*>(input.data)+size_t(y)*input.rowbytes);
        for(int x=0;x<input.width;++x) {
            const auto& p=row[x]; auto at=(size_t(y)*input.width+x)*3;
            const float a=p.alpha/scale;
            rgb[at]=a>0?p.red/(scale*a):0;rgb[at+1]=a>0?p.green/(scale*a):0;rgb[at+2]=a>0?p.blue/(scale*a):0;
        }
    }
}
template<class Pixel> void unpackCPU(const PF_LayerDef& input,PF_LayerDef& output,const std::vector<float>& rgb,float mix,float scale) {
    using Channel=decltype(Pixel{}.red);
    for(int y=0;y<input.height;++y) {
        const auto* src=reinterpret_cast<const Pixel*>(reinterpret_cast<const char*>(input.data)+size_t(y)*input.rowbytes);
        auto* dst=reinterpret_cast<Pixel*>(reinterpret_cast<char*>(output.data)+size_t(y)*output.rowbytes);
        for(int x=0;x<input.width;++x) {
            auto at=(size_t(y)*input.width+x)*3; const float a=src[x].alpha/scale; dst[x].alpha=src[x].alpha;
            auto cv=[&](float v,Channel old){ return Channel(std::round(std::clamp((1-mix)*old+mix*v*a*scale,0.0f,scale))); };
            dst[x].red=cv(rgb[at],src[x].red);dst[x].green=cv(rgb[at+1],src[x].green);dst[x].blue=cv(rgb[at+2],src[x].blue);
        }
    }
}
PF_Err renderImage(PF_InData* in_data,PF_OutData* out_data,PF_ParamDef* params[],PF_LayerDef* output) {
    PF_LayerDef& input=params[PIX_INPUT]->u.ld;
    auto copy=[&]{return PF_COPY(&input,output,nullptr,nullptr);};
    if(!input.data||!output||!output->data) return PF_Err_BAD_CALLBACK_PARAM;
    const float mix=float(params[MIX]->u.fs_d.value/100);
    if(mix<=0 || input.width<=0 || input.height<=0) return copy();
    try {
        if(input.width!=output->width||input.height!=output->height) throw std::runtime_error("Input/output dimensions differ");
        const int pixelSize=PF_WORLD_IS_DEEP(&input)?8:4;
        if(PF_WORLD_IS_DEEP(&input)!=PF_WORLD_IS_DEEP(output) || input.rowbytes<int64_t(input.width)*pixelSize || output->rowbytes<int64_t(output->width)*pixelSize)
            throw std::runtime_error("Invalid image stride or mismatched depth");
        auto config=readConfig(in_data,params[CONFIG]->u.arb_d.value); pix::ConfigHeader h;std::memcpy(&h,config.data(),sizeof(h));
        if(!h.xmlBytes) return copy();
        pix::Request r;r.width=input.width;r.height=input.height;r.configBytes=uint32_t(config.size());
        r.inputDb=float(params[INPUT_GAIN]->u.fs_d.value);r.bpm=float(params[TEMPO]->u.fs_d.value);
        for(unsigned i=0;i<pix::slots;++i) r.values[i]=float(params[SLOT_FIRST+i]->u.fs_d.value/100);
        pix::validate(r);
        std::vector<float> rgb(size_t(input.width)*input.height*3),result(rgb.size());
        int depth=PF_WORLD_IS_DEEP(&input)?16:8;bool gpu=params[GPU]->u.bd.value!=0;
        if(gpu) { if(!PixPackCUDA(input.data,input.width,input.height,input.rowbytes,depth,rgb.data())) throw std::runtime_error("CUDA transfer failed; input was preserved. Disable CUDA Pixel Transfer to use CPU."); }
        else if(depth==16) packCPU<PF_Pixel16>(input,rgb,32768);else packCPU<PF_Pixel8>(input,rgb,255);
        {
            std::unique_lock<std::mutex> guard(engineMutex,std::try_to_lock);
            if(!guard.owns_lock())return PF_Interrupt_CANCEL;
            loadEngine();char error[240]{};
            if(renderFn(&r,config.data(),rgb.data(),result.data(),error,sizeof(error))) throw std::runtime_error(error);
        }
        PF_Err cancel=PF_ABORT(in_data);if(cancel)return cancel;
        if(gpu) { if(!PixUnpackCUDA(input.data,output->data,input.width,input.height,input.rowbytes,output->rowbytes,depth,result.data(),mix)) throw std::runtime_error("CUDA output transfer failed; input was preserved"); }
        else if(depth==16) unpackCPU<PF_Pixel16>(input,*output,result,mix,32768);else unpackCPU<PF_Pixel8>(input,*output,result,mix,255);
        return PF_Err_NONE;
    } catch(const std::exception& e) {message(out_data,e.what());return copy();}
}
}
extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr,PF_PluginDataCB2 inPluginDataCallBackPtr,SPBasicSuite*,const char*,const char*) {
    PF_Err result=PF_Err_INVALID_CALLBACK;
    result=PF_REGISTER_EFFECT_EXT2(inPtr,inPluginDataCallBackPtr,"PixSpectra","mknb:PixSpectra","mknb_Plugin",AE_RESERVED_INFO,"EffectMain","https://www.adobe.com");return result;
}
PF_Err EffectMain(PF_Cmd cmd,PF_InData* in_data,PF_OutData* out_data,PF_ParamDef* params[],PF_LayerDef* output,void* extra) {
    try {
        switch(cmd) {
        case PF_Cmd_ABOUT: strcpy_s(out_data->return_msg,"PixSpectra 1.0\rImage / VST3 spectral processing inside After Effects.");break;
        case PF_Cmd_GLOBAL_SETUP:
            out_data->my_version=PF_VERSION(MAJOR_VERSION,MINOR_VERSION,BUG_VERSION,STAGE_VERSION,BUILD_VERSION);
            out_data->out_flags=PF_OutFlag_DEEP_COLOR_AWARE|PF_OutFlag_SEND_UPDATE_PARAMS_UI;
            out_data->out_flags2=0;break;
        case PF_Cmd_GLOBAL_SETDOWN: {
            std::lock_guard<std::mutex> guard(engineMutex);
            if(engineModule){shutdownFn();FreeLibrary(engineModule);engineModule=nullptr;}break;
        }
        case PF_Cmd_PARAMS_SETUP:return paramsSetup(in_data,out_data);
        case PF_Cmd_ARBITRARY_CALLBACK:return arbitrary(in_data,static_cast<PF_ArbParamsExtra*>(extra));
        case PF_Cmd_UPDATE_PARAMS_UI:return updateUI(in_data,params);
        case PF_Cmd_USER_CHANGED_PARAM:
            if(static_cast<PF_UserChangedParamExtra*>(extra)->param_index==SETUP)return configure(in_data,out_data,params);break;
        case PF_Cmd_RENDER:return renderImage(in_data,out_data,params,output);
        default:break;
        }
    } catch(const std::bad_alloc&) {return PF_Err_OUT_OF_MEMORY;}
    catch(const std::exception& e){message(out_data,e.what());return PF_Err_INTERNAL_STRUCT_DAMAGED;}
    catch(PF_Err e){return e;}
    return PF_Err_NONE;
}
