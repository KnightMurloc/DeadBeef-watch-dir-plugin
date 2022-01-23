//
// Created by victor on 22.01.2022.
//

#ifndef INOTIFY_TEST_UTILS_H
#define INOTIFY_TEST_UTILS_H
typedef enum {
    STR2INT_SUCCESS,
    STR2INT_OVERFLOW,
    STR2INT_UNDERFLOW,
    STR2INT_INCONVERTIBLE
} str2int_errno;

str2int_errno str2int(int *out, char *s, int base);
const char *get_filename_ext(const char *filename);
char* full_path(const char* path, const char* file);
#endif //INOTIFY_TEST_UTILS_H
