#ifndef UNISTD_H
#define UNISTD_H
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif
#endif