#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include <dirent.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

int compare_paths(const void *a, const void *b) {
    const char *path_a = a;
    const char *path_b = b;

    return strcmp(path_a, path_b);
}

int find_config_files(const char *input_dir, ConfigFiles *config_files){
  DIR *dir = opendir(input_dir);

  if (dir == NULL) {
    return 1;
  }

  config_files->count = 0;

  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL) {
    
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    size_t len = strlen(entry->d_name);

    if (len < 5 || strcmp(entry->d_name + len - 5, ".conf") != 0) {
      continue;
    }


    snprintf(config_files->paths[config_files->count], MAX_PATH_SIZE, "%s/%s", input_dir, entry->d_name);

    config_files->count++; 
  }
  qsort(config_files->paths,config_files->count, sizeof(config_files->paths[0]), compare_paths);
  
  closedir(dir);

  return 0;
}