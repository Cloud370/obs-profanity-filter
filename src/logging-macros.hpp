#pragma once

#include <obs-module.h>

// Global unified log prefix
#define BLOG(level, format, ...) blog(level, "[Profanity Filter] " format, ##__VA_ARGS__)
