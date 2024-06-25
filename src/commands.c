// #include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "commands.h"
#include "fat16.h"
#include "support.h"

off_t fsize(const char *filename)
{
    struct stat st;
    if (stat(filename, &st) == 0)
        return st.st_size;
    return -1;
}

struct fat_dir find(struct fat_dir *dirs, char *filename, struct fat_bpb *bpb)
{
    struct fat_dir curdir;
    int dirs_len = sizeof(struct fat_dir) * bpb->possible_rentries;
    int i;
    for (i = 0; i < dirs_len; i++)
    {
        if (strcmp((char *)dirs[i].name, filename) == 0)
        {
            curdir = dirs[i];
            break;
        }
    }
    return curdir;
}

struct fat_dir *ls(FILE *fp, struct fat_bpb *bpb)
{
    int i;
    struct fat_dir *dirs = malloc(sizeof(struct fat_dir) * bpb->possible_rentries);
    for (i = 0; i < bpb->possible_rentries; i++)
    {
        uint32_t offset = bpb_froot_addr(bpb) + i * 32;
        read_bytes(fp, offset, &dirs[i], sizeof(dirs[i]));
    }
    return dirs;
}

int write_dir(FILE *fp, char *fname, struct fat_dir *dir)
{
    char *name = padding(fname);
    strcpy((char *)dir->name, (char *)name);
    if (fwrite(dir, 1, sizeof(struct fat_dir), fp) <= 0)
        return -1;
    return 0;
}

int write_data(FILE *fp, char *fname, struct fat_dir *dir, struct fat_bpb *bpb)
{
    printf("%d", bpb->possible_rentries);
    struct fat_dir *dirs = ls(fp, &bpb);
    FILE *localf = fopen(fname, "r");
    int c;

    while ((c = fgetc(localf)) != EOF)
    {
        if (fputc(c, fp) != c)
            return -1;
    }
    return 0;
}

int wipe(FILE *fp, struct fat_dir *dir, struct fat_bpb *bpb)
{
    int start_offset = bpb_froot_addr(bpb) + (bpb->bytes_p_sect *
                                              dir->starting_cluster);
    int limit_offset = start_offset + dir->file_size;

    while (start_offset <= limit_offset)
    {
        fseek(fp, ++start_offset, SEEK_SET);
        if (fputc(0x0, fp) != 0x0)
            return 01;
    }
    return 0;
}

void mv(FILE *fp, char *path, struct fat_bpb *bpb) {
    // 1. Abrir o arquivo de origem
    FILE *src_file = fopen(path, "rb");
    if (!src_file) {
        fprintf(stderr, "Erro ao abrir o arquivo de origem: %s\n", path);
        return;
    }

    // 2. Obter o tamanho do arquivo de origem
    fseek(src_file, 0, SEEK_END);
    long file_size = ftell(src_file);
    fseek(src_file, 0, SEEK_SET);

    // 3. Ler o conteúdo do arquivo de origem
    unsigned char *buffer = (unsigned char *)malloc(file_size);
    if (!buffer) {
        fprintf(stderr, "Erro ao alocar memória para o buffer\n");
        fclose(src_file);
        return;
    }
    fread(buffer, 1, file_size, src_file);

    // 4. Fechar o arquivo de origem
    fclose(src_file);

    // 5. Encontrar um slot livre no diretório raiz do FAT16
    struct fat_dir *dirs = ls(fp, bpb);
    int i;
    int free_index = -1;
    for (i = 0; i < bpb->possible_rentries; i++) {
        if (dirs[i].name[0] == 0x00 || dirs[i].name[0] == 0xE5) {
            free_index = i;
            break;
        }
    }

    if (free_index == -1) {
        fprintf(stderr, "Erro: Diretório raiz está cheio\n");
        free(buffer);
        return;
    }

    // 6. Preparar a entrada do diretório para o novo arquivo
    struct fat_dir new_dir;
    memset(&new_dir, 0, sizeof(struct fat_dir));
    char *name = padding(path);
    strncpy((char *)new_dir.name, name, 11);
    new_dir.file_size = file_size;
    new_dir.starting_cluster = free_index + 2;  // Assumindo que o cluster inicial está no índice livre

    // 7. Escrever a entrada do diretório no FAT16
    uint32_t dir_offset = bpb_froot_addr(bpb) + free_index * sizeof(struct fat_dir);
    fseek(fp, dir_offset, SEEK_SET);
    fwrite(&new_dir, sizeof(struct fat_dir), 1, fp);

    // 8. Escrever os dados do arquivo no FAT16
    uint32_t data_offset = bpb_fdata_addr(bpb) + new_dir.starting_cluster * bpb->bytes_p_sect;
    fseek(fp, data_offset, SEEK_SET);
    fwrite(buffer, 1, file_size, fp);

    // 9. Liberar o buffer
    free(buffer);

    // 10. Remover o arquivo de origem
    if (remove(path) != 0) {
        fprintf(stderr, "Erro ao remover o arquivo de origem: %s\n", path);
    }

    // 11. Limpar a memória alocada para a lista de diretórios
    free(dirs);
}

void rm(FILE *fp, char *filename, struct fat_bpb *bpb)
{
    // 1. Localizar o arquivo na imagem FAT16
    struct fat_dir *dirs = ls(fp, bpb);
    struct fat_dir *file_dir = NULL;

    for (int i = 0; i < bpb->possible_rentries; i++)
    {
        if (strncmp((char *)dirs[i].name, filename, 11) == 0)
        {
            file_dir = &dirs[i];
            break;
        }
    }

    if (!file_dir)
    {
        fprintf(stderr, "File %s not found in the FAT16 image\n", filename);
        free(dirs);
        return;
    }

    // 2. Marcar a entrada do diretório como livre
    file_dir->name[0] = DIR_FREE_ENTRY;

    uint32_t dir_offset = bpb_froot_addr(bpb) + ((unsigned char *)file_dir - (unsigned char *)dirs);
    fseek(fp, dir_offset, SEEK_SET);
    fwrite(file_dir, sizeof(struct fat_dir), 1, fp);

    // 3. Liberar os clusters na FAT
    uint32_t cluster = file_dir->starting_cluster;

    while (cluster < 0xFFF8)
    { // FAT16 end-of-chain markers range from 0xFFF8 to 0xFFFF
        uint32_t next_cluster;
        uint32_t fat_offset = bpb_faddress(bpb) + cluster * 2;

        fseek(fp, fat_offset, SEEK_SET);
        fread(&next_cluster, 2, 1, fp);

        // Liberar o cluster na FAT (não usei free_fat_entry neste exemplo)
        uint16_t free_value = 0x0000;
        fseek(fp, fat_offset, SEEK_SET);
        fwrite(&free_value, 2, 1, fp);

        if (next_cluster >= 0xFFF8)
        {
            break;
        }

        cluster = next_cluster;
    }

    // 4. Limpar os dados do arquivo (opcional, dependendo da implementação)
    int result = wipe(fp, file_dir, bpb);
    if (result != 0)
    {
        fprintf(stderr, "Failed to wipe file data\n");
        free(dirs);
        return;
    }

    free(dirs);
    printf("Arquivo %s removido!\n", filename);
}

void cp(FILE *fp, char *filename, char *dest, struct fat_bpb *bpb)
{
    struct fat_dir *dirs = ls(fp, bpb);

    struct fat_dir dir = find(dirs, filename, bpb);

    FILE *dest_fp = fopen(dest, "wb");
    if (!dest_fp)
    {
        fprintf(stderr, "Could not open destination file %s\n", dest);
        return;
    }

    uint32_t data_addr = bpb_fdata_addr(bpb) + (dir.starting_cluster - 2) * bpb->bytes_p_sect;
    char *buffer = malloc(dir.file_size);
    if (read_bytes(fp, data_addr, buffer, dir.file_size) != 0)
    {
        fprintf(stderr, "Failed to read file %s from image\n", filename);
        free(buffer);
        fclose(dest_fp);
        return;
    }

    if (fwrite(buffer, 1, dir.file_size, dest_fp) != dir.file_size)
    {
        fprintf(stderr, "Failed to write to destination file %s\n", dest);
    }

    free(buffer);
    fclose(dest_fp);
}
