#pragma once
#define NOMINMAX
#define PF_DEEP_COLOR_AWARE 1
#include "AEConfig.h"
#include <Windows.h>
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AEGP_SuiteHandler.h"
#include "PixSpectra_Strings.h"
#include "EngineAPI.h"

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 5
enum { PIX_INPUT=0, LEGACY_GAIN, LEGACY_COLOR, CONFIG, SETUP, MIX, INPUT_GAIN, TEMPO, GPU,
       SLOT_FIRST, NUM_PARAMS=SLOT_FIRST+pix::slots };
enum { CONFIG_ID=100, SETUP_ID, MIX_ID, INPUT_GAIN_ID, TEMPO_ID, GPU_ID, SLOT_ID=200 };
extern "C" DllExport PF_Err EffectMain(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef*[],PF_LayerDef*,void*);
extern "C" bool PixPackCUDA(const void*,int,int,int,int,float*);
extern "C" bool PixUnpackCUDA(const void*,void*,int,int,int,int,int,const float*,float);
