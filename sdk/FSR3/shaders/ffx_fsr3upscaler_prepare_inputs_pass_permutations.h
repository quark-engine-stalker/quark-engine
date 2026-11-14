#include "ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef.h"
#include "ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe.h"

typedef union ffx_fsr3upscaler_prepare_inputs_pass_PermutationKey {
    struct {
        uint32_t FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING : 1;
    };
    uint32_t index;
} ffx_fsr3upscaler_prepare_inputs_pass_PermutationKey;

typedef struct ffx_fsr3upscaler_prepare_inputs_pass_PermutationInfo {
    const uint32_t       blobSize;
    const unsigned char* blobData;


    const uint32_t  numConstantBuffers;
    const char**    constantBufferNames;
    const uint32_t* constantBufferBindings;
    const uint32_t* constantBufferCounts;
    const uint32_t* constantBufferSpaces;

    const uint32_t  numSRVTextures;
    const char**    srvTextureNames;
    const uint32_t* srvTextureBindings;
    const uint32_t* srvTextureCounts;
    const uint32_t* srvTextureSpaces;

    const uint32_t  numUAVTextures;
    const char**    uavTextureNames;
    const uint32_t* uavTextureBindings;
    const uint32_t* uavTextureCounts;
    const uint32_t* uavTextureSpaces;

    const uint32_t  numSRVBuffers;
    const char**    srvBufferNames;
    const uint32_t* srvBufferBindings;
    const uint32_t* srvBufferCounts;
    const uint32_t* srvBufferSpaces;

    const uint32_t  numUAVBuffers;
    const char**    uavBufferNames;
    const uint32_t* uavBufferBindings;
    const uint32_t* uavBufferCounts;
    const uint32_t* uavBufferSpaces;

    const uint32_t  numSamplers;
    const char**    samplerNames;
    const uint32_t* samplerBindings;
    const uint32_t* samplerCounts;
    const uint32_t* samplerSpaces;

    const uint32_t  numRTAccelerationStructures;
    const char**    rtAccelerationStructureNames;
    const uint32_t* rtAccelerationStructureBindings;
    const uint32_t* rtAccelerationStructureCounts;
    const uint32_t* rtAccelerationStructureSpaces;
} ffx_fsr3upscaler_prepare_inputs_pass_PermutationInfo;

static const uint32_t g_ffx_fsr3upscaler_prepare_inputs_pass_IndirectionTable[] = {
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
    7,
    3,
    6,
    2,
    5,
    1,
    4,
    0,
};

static const ffx_fsr3upscaler_prepare_inputs_pass_PermutationInfo g_ffx_fsr3upscaler_prepare_inputs_pass_PermutationInfo[] = {
    { g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_size, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_9a56c7714dc927876bcabeaaf5ee975c_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_size, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_c64e2fc3b6234df391eeaeed5018a577_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_size, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_15109ba0400fa6d91977f1c13ebd4a56_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_size, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_5d4b7029e162620b8857c34f2e7dc17c_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_size, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_0c75d7251c41ce17ec3086c51f844973_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_size, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_ad6f76c6ae51af212076922af554678c_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_size, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_579a80ebaed03193452dc2d9cb97feef_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
    { g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_size, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_data, 1, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_CBVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_CBVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_CBVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_CBVResourceSpaces, 3, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureSRVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureSRVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureSRVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureSRVResourceSpaces, 5, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureUAVResourceNames, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureUAVResourceBindings, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureUAVResourceCounts, g_ffx_fsr3upscaler_prepare_inputs_pass_4fc175089afb00c9b8d3c264ec4a23fe_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
};

