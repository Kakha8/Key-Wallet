#pragma once
#include <stdbool.h>
void wallet_ui_init(void);
bool wallet_ui_prompt(void);
void wallet_ui_result(const char *message);
void wallet_ui_task(void);
void wallet_ui_command(const char *command);
