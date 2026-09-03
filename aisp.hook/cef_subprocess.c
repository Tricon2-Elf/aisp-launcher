#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "include/capi/cef_app_capi.h"
#include "include/cef_api_hash.h"

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_command) {
  (void)previous;
  (void)command_line;
  (void)show_command;

  cef_api_hash(CEF_API_VERSION, 0);
  cef_main_args_t arguments = {};
  arguments.instance = instance;
  return cef_execute_process(&arguments, NULL, NULL);
}
