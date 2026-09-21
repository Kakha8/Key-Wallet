#pragma once
#include <stdbool.h>
void wallet_ui_init(void);
void wallet_ui_reset_on_boot(void);
bool wallet_ui_prompt(void);
int wallet_ui_take_local_authorization(void);
void wallet_ui_result(const char *message);
void wallet_ui_task(void);
void wallet_ui_command(const char *command);
