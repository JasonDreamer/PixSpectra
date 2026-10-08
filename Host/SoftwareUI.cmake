# Scoped to PixSpectra's pinned JUCE checkout. Apply before JUCE compilation so
# no temporary Direct2D peer/image is created before switching to software.
function(pix_patch_juce relative before after)
    set(path "${CMAKE_CURRENT_LIST_DIR}/../third_party/JUCE/${relative}")
    file(READ "${path}" source)
    string(FIND "${source}" "${after}" already)
    if(NOT already EQUAL -1)
        return()
    endif()
    string(FIND "${source}" "${before}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "PixSpectra software UI patch does not match pinned JUCE: ${relative}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${path}" "${source}")
endfunction()

pix_patch_juce("modules/juce_gui_basics/native/juce_Windowing_windows.cpp"
    "    return new HWNDComponentPeer { *this, styleFlags, (HWND) parentHWND, false, 1 };"
    "   #if PIXSPECTRA_SOFTWARE_UI\n    return new HWNDComponentPeer { *this, styleFlags, (HWND) parentHWND, false, 0 };\n   #else\n    return new HWNDComponentPeer { *this, styleFlags, (HWND) parentHWND, false, 1 };\n   #endif")

pix_patch_juce("modules/juce_graphics/native/juce_Direct2DImage_windows.cpp"
    "ImagePixelData::Ptr NativeImageType::create (Image::PixelFormat format, int width, int height, bool clearImage) const\n{"
    "ImagePixelData::Ptr NativeImageType::create (Image::PixelFormat format, int width, int height, bool clearImage) const\n{\n   #if PIXSPECTRA_SOFTWARE_UI\n    return new SoftwarePixelData { format, width, height, clearImage };\n   #endif")
