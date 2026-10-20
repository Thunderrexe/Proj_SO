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
#include <errno.h>

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

int find_config_files(const char *input_dir, ConfigFiles *config_files) {
    if (input_dir == NULL || config_files == NULL) {
        return 1;
    }

    DIR *dir = opendir(input_dir);
    if (dir == NULL) {
        perror("opendir");
        return 1;
    }

    config_files->count = 0;

    size_t capacity = sizeof(config_files->paths) / sizeof(config_files->paths[0]);

    int result = 0;
    struct dirent *entry;

    while (1) {
        errno = 0;
        entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0) {
                perror("readdir");
                result = 1;
            }
            break;
        }

        /* Ignorar as entradas "." e "..". */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Aceitar apenas nomes terminados em ".conf". */
        size_t len = strlen(entry->d_name);

        if (len < 5 || strcmp(entry->d_name + len - 5, ".conf") != 0) {
            continue;
        }

        /* Construir o caminho completo e verificar o seu tamanho. */
        char path[MAX_PATH_SIZE];

        int n = snprintf(path, sizeof(path), "%s/%s", input_dir, entry->d_name);

        if (n < 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Configuration path too long.\n");
            result = 1;
            break;
        }

        /* Confirmar que a entrada é um ficheiro regular. */
        struct stat st;

        if (stat(path, &st) != 0) {
            perror("stat");
            result = 1;
            break;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        /* Evitar ultrapassar o array de caminhos. */
        if (config_files->count >= capacity) {
            fprintf(stderr, "Too many configuration files.\n");
            result = 1;
            break;
        }

        strcpy(config_files->paths[config_files->count], path);
        config_files->count++;
    }

    if (closedir(dir) != 0) {
        perror("closedir");
        result = 1;
    }

    if (result != 0) {
        config_files->count = 0;
        return 1;
    }

    /* Ordenar alfabeticamente os caminhos. */
    qsort(config_files->paths,
          config_files->count,
          sizeof(config_files->paths[0]),
          compare_paths);

    return 0;
}