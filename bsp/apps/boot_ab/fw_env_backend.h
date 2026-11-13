#ifndef SGW_FW_ENV_BACKEND_H
#define SGW_FW_ENV_BACKEND_H

#include "ab_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *fw_printenv_bin;
    const char *fw_setenv_bin;
    const char *config_path;
} fw_env_backend_cfg_t;

void fw_env_backend_cfg_from_env(fw_env_backend_cfg_t *cfg);

int fw_env_backend_load(const fw_env_backend_cfg_t *cfg,
                        ab_env_t *env,
                        char *errbuf,
                        size_t errlen);

int fw_env_backend_save(const fw_env_backend_cfg_t *cfg,
                        const ab_env_t *env,
                        char *errbuf,
                        size_t errlen);

int fw_env_backend_check_script_line(const char *line,
                                     char *errbuf,
                                     size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* SGW_FW_ENV_BACKEND_H */
