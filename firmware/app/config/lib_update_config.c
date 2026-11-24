/*
 * Derived from the JieLi AC63 SDK (Apache License 2.0), apps/hid/config/lib_update_config.c,
 * commit fdc018de81629e21e594d677cf05f87d288be0f0. See NOTICE.
 * Changes: every SDK update mode is off (config_update_mode = 0). Our firmware is updated only
 * through our DFU bootloader; the SDK's paths (a JieLi test box over BLE or Bluetooth LMP, the
 * RCSP phone app, UART) would rewrite it around the bootloader (SPEC R-13). The ROM download mode
 * and the uboot's UART update do not depend on this setting.
 */
#include "app_config.h"
#include "system/includes.h"
#include "update_loader_download.h"

const int config_update_mode = 0;

//是否采用双备份升级方案:0-单备份;1-双备份
#if CONFIG_DOUBLE_BANK_ENABLE
const int support_dual_bank_update_en = 1;
#else
const int support_dual_bank_update_en = 0;
#endif

//是否支持外挂flash升级,需要打开Board.h中的TCFG_NOR_FS_ENABLE
const int support_norflash_update_en  = 0;

//支持从外挂flash读取ufw文件升级使能
const int support_norflash_ufw_update_en = 0;

#if OTA_TWS_SAME_TIME_NEW       //使用新的同步升级流程
const int support_ota_tws_same_time_new =  1;
#else
const int support_ota_tws_same_time_new =  0;
#endif

//是否支持升级之后保留vm数据
const int support_vm_data_keep = 1;

const char log_tag_const_v_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  FALSE;
const char log_tag_const_i_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  FALSE;
const char log_tag_const_d_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  FALSE;
const char log_tag_const_w_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  TRUE;
const char log_tag_const_e_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  TRUE;
