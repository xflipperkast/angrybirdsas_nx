#ifndef ASNX_CONFIG_H
#define ASNX_CONFIG_H

#define GAME_HOME        "sdmc:/switch/angrybirdas_nx"
#define INSTALL_APK      GAME_HOME "/game.apk"
#define DATA_ROOT        GAME_HOME "/runtime"
#define INSTALL_MARKER   DATA_ROOT "/.installed-1.2.8.1"
#define LOG_PATH         GAME_HOME "/angrybirdas_crash.log"
#define GAME_PACKAGE     "com.dripmissouri.refreshed"
#define ANDROID_DATA_DIR "/data/data/" GAME_PACKAGE
#define ANDROID_FILES_DIR ANDROID_DATA_DIR "/files"
#define ANDROID_CACHE_DIR ANDROID_DATA_DIR "/cache"
#define ANDROID_EXTERNAL_FILES_DIR "/storage/emulated/0/Android/data/" GAME_PACKAGE "/files"
#define UNITY_VERSION    "6000.3.5f2"
#define EXPECTED_MAIN_BUILD_ID   "c5498ab8c791f1473a54c79a9d3c5b738ff6d941"
#define EXPECTED_UNITY_BUILD_ID  "2ce7c7e5629051e2f167b6bb620572513b7ae99e"
#define EXPECTED_IL2CPP_BUILD_ID "cc964e2dd22aae8062385efeb6df69e02e65090c"

#define LIB_MAIN         "libmain.so"
#define LIB_UNITY        "libunity.so"
#define LIB_IL2CPP       "libil2cpp.so"

#define OFF_IL2CPP_GC_SUSPEND_HANDLER          0x010AE688u
#define OFF_IL2CPP_GC_RESTART_HANDLER          0x010AE7FCu
#define OFF_IL2CPP_GC_THREAD_TABLE              0x02C2BF28u
#define OFF_IL2CPP_GC_WORLD_MARKER              0x02C2BEF8u
#define OFF_IL2CPP_GC_ACK_SEM                   0x02C2BF08u
#define OFF_IL2CPP_GC_RESTART_ACK_ENABLED       0x02A0B510u

#define OFF_UNITY_INIT_JNI                   0x00AAC578u
#define OFF_UNITY_APP_UNLOAD                 0x00AAC7A0u
#define OFF_UNITY_INJECT_EVENT               0x00AACB54u
#define OFF_UNITY_RENDER                     0x00AACAF4u
#define OFF_UNITY_PAUSE                      0x00AAC6DCu
#define OFF_UNITY_RESUME                     0x00AAC740u
#define OFF_UNITY_DONE                       0x00AAC5FCu
#define OFF_UNITY_FOCUS_CHANGED              0x00AAC7F0u
#define OFF_UNITY_RECREATE_GFX               0x00AAC854u
#define OFF_UNITY_SURFACE_CHANGED            0x00AAC8BCu
#define OFF_UNITY_PLAYER_SET_RUNNING         0x00AAD6C8u

#define OFF_UNITY_CPU_COUNT_RETURN           0x01886BDCu
#define UNITY_CPU_COUNT_RETURN_ORIG          0xB9400660u

#define OFF_UNITY_FMOD_GET_INFO              0x019D2574u
#define OFF_UNITY_FMOD_PROCESS               0x019D263Cu
#define OFF_UNITY_FMOD_PROCESS_MIC_DATA      0x019D26C8u

#define OFF_TIME_MANAGER_UPDATE_ENTRY        0x008CF61Cu
#define OFF_TIME_MANAGER_UPDATE_BODY         0x008CF640u
#define OFF_TM_FRAMECOUNT_U64                 0x160u
#define OFF_TM_AUX_U32                        0x168u
#define OFF_TM_PAUSE_U8                       0x1A8u
#define OFF_VSYNC_COUNTER                     0x01B3D460u
#define OFF_CHOREOGRAPHER_WAIT_SITE           0x00A9F4A0u
#define CHOREOGRAPHER_WAIT_FROM               0x17FFFFF9u
#define CHOREOGRAPHER_WAIT_TO                 0x14000001u

#define ENABLE_FMOD_AUDIO                    0
#define ENABLE_TOUCH_INPUT                   1
#define ENABLE_IL2CPP_GC_DISABLE             0
#define ENABLE_UNITY_TIME_FIX                 1
#define ENABLE_CHOREOGRAPHER_WAIT_PATCH       1
#define ENABLE_DYNAMIC_HEAP_REGION_PATCH       1

#define OFF_DYNAMIC_HEAP_REGION_SIZE             0x01007878u
#define DYNAMIC_HEAP_REGION_256M_INSN            0x52A20009u
#define DYNAMIC_HEAP_REGION_64M_INSN             0x52A08009u
#define OFF_DYNAMIC_HEAP_REGION_MINUS_ONE         0x01007D44u
#define DYNAMIC_HEAP_MINUS1_256M_INSN            0x12BE000Du
#define DYNAMIC_HEAP_MINUS1_64M_INSN             0x12BF800Du
#define OFF_DYNAMIC_HEAP_REGION_ALIGN_MASK        0x01007D70u
#define DYNAMIC_HEAP_MASK_256M_INSN              0x92648D36u
#define DYNAMIC_HEAP_MASK_64M_INSN               0x92669536u
#define OFF_DYNAMIC_HEAP_VM_ALIGNMENT            0x010095A4u
#define DYNAMIC_HEAP_ALIGN_256M_INSN             0x52A20008u
#define DYNAMIC_HEAP_ALIGN_64M_INSN              0x52A08008u

#define OFF_OWNER_RANGE_ADDR_MASK                0x010094F4u
#define OWNER_RANGE_MASK_56_INSN                 0x9240DC28u
#define OWNER_RANGE_MASK_54_INSN                 0x9240D428u
#define OFF_OWNER_RANGE_START_BUCKET             0x010094F8u
#define OWNER_START_28_INSN                      0xD35CDC33u
#define OWNER_START_26_INSN                      0xD35AD433u
#define OFF_OWNER_RANGE_END_BUCKET               0x01009500u
#define OWNER_END_28_INSN                        0xD35CFD15u
#define OWNER_END_26_INSN                        0xD35AFD15u
#define OFF_OWNER_LOOKUP_BUCKET                  0x010097D4u
#define OWNER_LOOKUP_28_INSN                     0xD35CFC28u
#define OWNER_LOOKUP_26_INSN                     0xD35AFC28u
#define OFF_OWNER_LOOKUP_BASE_MASK               0x010097E4u
#define OWNER_BASE_MASK_28_INSN                  0x92646C28u
#define OWNER_BASE_MASK_26_INSN                  0x92666C28u
#define OFF_OWNER_LOOKUP_LEAF_INDEX              0x010097ECu
#define OWNER_LEAF_28_INSN                       0xD35C9C2Au
#define OWNER_LEAF_26_INSN                       0xD35A942Au
#define OFF_OWNER_LOOKUP_REGION_INDEX            0x01009800u
#define OWNER_REGION_28_INSN                     0xD35CDC29u
#define OWNER_REGION_26_INSN                     0xD35AD429u
#define OFF_OWNER_RUN_BACKSTEP_HI                0x01009804u
#define OWNER_BACKSTEP_HI_256M_INSN              0xB25C6FEBu
#define OWNER_BACKSTEP_HI_64M_INSN               0xB25E77EBu
#define OFF_OWNER_RUN_BACKSTEP_LO                0x01009808u
#define OWNER_BACKSTEP_LO_256M_INSN              0xF2A2000Bu
#define OWNER_BACKSTEP_LO_64M_INSN               0xF2A0800Bu
#define OFF_OWNER_LOOKUP_BACKSTEP                0x0100984Cu
#define OWNER_BACKSTEP_SHIFT28_INSN              0xCB0A7108u
#define OWNER_BACKSTEP_SHIFT26_INSN              0xCB0A6908u
#define OFF_OWNER_DIRECT_TOP_INDEX               0x01009860u
#define OWNER_TOP_SHIFT40_INSN                   0xD368FC28u
#define OWNER_TOP_SHIFT38_INSN                   0xD366FC28u
#define OFF_OWNER_DIRECT_LEAF_INDEX              0x01009870u
#define OWNER_DIRECT_LEAF28_INSN                 0xD35C9C29u
#define OWNER_DIRECT_LEAF26_INSN                 0xD35A9429u
#define OFF_OWNER_MANAGER_TOP_INDEX              0x0100B2A4u
#define OWNER_MANAGER_TOP40_INSN                 0xD368FC28u
#define OWNER_MANAGER_TOP38_INSN                 0xD366FC28u
#define OFF_OWNER_MANAGER_LEAF_INDEX             0x0100B2BCu
#define OWNER_MANAGER_LEAF28_INSN                0xD35C9E89u
#define OWNER_MANAGER_LEAF26_INSN                0xD35A9689u

#define SO_ARENA_BYTES          (128ull * 1024ull * 1024ull)

#define UNITY_MMAP_ARENA_BYTES  (2112ull * 1024ull * 1024ull)
#define UNITY_MMAP_SLOT_BYTES   (64ull * 1024ull * 1024ull)
#define MIN_NEWLIB_HEAP_BYTES   (768ull * 1024ull * 1024ull)
#define ZIP_IO_BYTES            (128u * 1024u)
#define UNITY_SESSION_STACK_BYTES (16u * 1024u * 1024u)

#endif
