#pragma once

#include "backend123.hpp"
#include <fuse/fuse_lowlevel.h>
#include <string>

void openfile_startscan();
void openfile_stopscan();
// openfile_register returns an fi->fh
uint64_t openfile_register(fuse_ino_t ino, const reply123& reply);
void openfile_release(fuse_ino_t ino, uint64_t fifh);
void openfile_expire_now(fuse_ino_t ino, uint64_t fifh);
std::string openfile_report();
