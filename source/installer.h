#ifndef ASNX_INSTALLER_H
#define ASNX_INSTALLER_H
int installer_prepare_game_files(void);
const char *installer_last_error(void);
const char *installer_package_name(void);
const char *installer_version_name(void);
int installer_version_code(void);
int installer_min_sdk(void);
int installer_target_sdk(void);
#endif
