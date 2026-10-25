#ifndef FILESYSTEM__H
#define FILESYSTEM__H

#include <stddef.h>
#include "constants.h"

#define MAX_CONFIG_FILES 100

/*
 * Armazena os caminhos completos dos ficheiros .conf encontrados.
 * count indica quantas posições de paths estão ocupadas. O vetor tem uma
 * capacidade fixa de MAX_CONFIG_FILES, pelo que deve ser validado antes de
 * inserir um novo caminho.
 */
typedef struct {
    char paths[MAX_CONFIG_FILES][MAX_PATH_SIZE];
    size_t count;
} ConfigFiles;

/**
 * Verifica se um caminho existe e corresponde a uma diretoria.
 *
 * @param path Caminho da diretoria.
 *
 * @return 1 se existir e for uma diretoria; 0 caso contrário.
 */
int path_exists(const char *path);

/**
 * Verifica se um caminho existe e corresponde a um ficheiro regular.
 *
 * @param path Caminho do ficheiro.
 *
 * @return 1 se existir e for um ficheiro regular; 0 caso contrário.
 */
int file_exists(const char *path);

/**
 * Converte o caminho indicado num caminho absoluto, resolvendo ligações
 * simbólicas, componentes relativos ('.' e '..') e separadores redundantes.
 * O caminho resolvido é copiado para a zona de memória fornecida.
 *
 * @param path Caminho a resolver.
 * @param buffer Zona de memória de destino onde será guardado o caminho absoluto.
 * @param size Tamanho da zona de memória de destino, em bytes.
 *
 * @return 0 se o caminho for resolvido e copiado com sucesso.
 * @return 1 se não for possível resolver o caminho ou se a zona de memória
 * de destino for demasiado pequena.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/**
 * Procura ficheiros regulares com extensão .conf na diretoria indicada,
 * guarda os respetivos caminhos completos em ConfigFiles e ordena-os por
 * ordem lexicográfica. Devolve 0 em caso de sucesso e 1 em caso de erro.
 * Se ocorrer um erro, count é reposto a zero.
 *
 * @param input_dir Diretoria a examinar.
 * @param config_files Estrutura que recebe os caminhos e a respetiva quantidade.
 *
 * @return 0 se a procura for concluída com sucesso; 1 se ocorrer um erro.
 */
int find_config_files(const char *input_dir, ConfigFiles *config_files);

#endif // FILESYSTEM__H