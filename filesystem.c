#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include <dirent.h>

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/*
 * Verifica se o caminho existe e corresponde a uma diretoria.
 * stat() também segue ligações simbólicas; se a consulta falhar, devolve 0.
 */
int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  /* O caminho pode existir sem ser uma diretoria, por isso é necessário
   * verificar o tipo indicado em st_mode. */
  return S_ISDIR(st.st_mode);
}

/* Verifica se o caminho existe e é um ficheiro regular, não apenas uma
 * entrada qualquer do sistema de ficheiros. */
int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

/* Comparador usado por qsort(): cada elemento ordenado é um caminho completo.
 * strcmp() define uma ordem lexicográfica determinística entre os caminhos. */
int compare_paths(const void *a, const void *b) {
    const char *path_a = a;
    const char *path_b = b;

    return strcmp(path_a, path_b);
}

/*
 * Descobre os ficheiros de configuração diretamente dentro de input_dir.
 * Guarda apenas ficheiros regulares com extensão .conf, limita a quantidade
 * à capacidade de ConfigFiles e ordena os caminhos no final para que a ordem
 * de processamento não dependa da ordem arbitrária devolvida por readdir().
 */
int find_config_files(const char *input_dir, ConfigFiles *config_files) {
    /* Validar os argumentos antes de os utilizar. */
    if (input_dir == NULL || config_files == NULL) {
        return 1;
    }

    DIR *dir = opendir(input_dir);
    if (dir == NULL) {
        perror("opendir");
        return 1;
    }

    /* Começar com uma lista vazia. A capacidade é obtida a partir do próprio
     * array, evitando duplicar o valor MAX_CONFIG_FILES neste cálculo. */    
    config_files->count = 0;

    size_t capacity = sizeof(config_files->paths) / sizeof(config_files->paths[0]);

    int result = 0;
    struct dirent *entry;

    while (1) {

        /* readdir() devolve NULL tanto no fim da diretoria como em caso de
         * erro. Limpar errno antes da chamada permite distinguir os casos. */    
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

        /* path já foi formatado num buffer limitado e verificado quanto a
         * truncagem. Copiá-lo para a próxima posição deixa sempre o array
         * terminado em NULL e só depois incrementa o número de caminhos. */
        strcpy(config_files->paths[config_files->count], path);
        config_files->count++;
    }

    /* A diretoria deve ser fechada mesmo que a leitura tenha falhado. */
    if (closedir(dir) != 0) {
        perror("closedir");
        result = 1;
    }

    if (result != 0) {
        
        /* Não deixar uma lista parcial aparentar ser um resultado válido. */        
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