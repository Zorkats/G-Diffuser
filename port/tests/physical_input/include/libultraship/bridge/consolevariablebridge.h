
#pragma once
extern "C" int CVarGetInteger(const char*, int);
extern "C" void CVarSetInteger(const char*, int);
extern "C" const char* CVarGetString(const char*, const char*);
extern "C" void CVarSetString(const char*, const char*);
extern "C" void CVarSave();
