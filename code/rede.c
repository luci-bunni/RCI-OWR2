#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rede.h"

void inicializar_vizinhos(struct Vizinho vizinhos[]) {
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        vizinhos[i].fd = -1;
        strcpy(vizinhos[i].id, "?");
    }
}

void mostrar_vizinhos(struct Vizinho vizinhos[]) {
    printf("--- Os teus Vizinhos Atuais ---\n");
    int found = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (vizinhos[i].fd != -1) {
            printf("-> Nó %s (no Slot %d)\n", vizinhos[i].id, i);
            found++;
        }
    }
    if (found == 0) printf("Ainda não tens vizinhos.\n");
    printf("-------------------------------\n");
}

void remover_aresta(struct Vizinho vizinhos[], char *target_id) {
    int found = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (vizinhos[i].fd != -1 && strcmp(vizinhos[i].id, target_id) == 0) {
            printf("[TCP] A cortar o cabo com o Nó %s...\n", target_id);
            close(vizinhos[i].fd);
            vizinhos[i].fd = -1;
            strcpy(vizinhos[i].id, "?");
            found = 1;
            printf("Aresta removida com sucesso!\n");
            break;
        }
    }
    if (!found) {
        printf("Erro: O Nó %s não é teu vizinho atual.\n", target_id);
    }
}

void aceitar_ligacao(int fd_tcp_listen, struct Vizinho vizinhos[]) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int new_fd = accept(fd_tcp_listen, (struct sockaddr*)&client_addr, &client_len);
    
    if (new_fd != -1) {
        int slot_livre = -1;
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (vizinhos[i].fd == -1) { slot_livre = i; break; }
        }

        if (slot_livre != -1) {
            vizinhos[slot_livre].fd = new_fd;
            strcpy(vizinhos[slot_livre].id, "?"); 
            printf("\n[TCP] Nova ligação aceite no Slot %d. À espera que se apresente...\n", slot_livre);
        } else {
            printf("\n[TCP] Rejeitado. Sem espaço para mais vizinhos.\n");
            close(new_fd);
        }
    } else {
        perror("Erro no accept");
    }
}

void ligar_vizinho(struct Vizinho vizinhos[], char *res_id, char *res_ip, int res_tcp, char *meu_id) {
    int fd_novo = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in viz_addr;
    memset(&viz_addr, 0, sizeof(viz_addr));
    viz_addr.sin_family = AF_INET;
    viz_addr.sin_port = htons(res_tcp);
    inet_pton(AF_INET, res_ip, &viz_addr.sin_addr);
    
    if (connect(fd_novo, (struct sockaddr*)&viz_addr, sizeof(viz_addr)) == -1) {
        perror("Erro a ligar");
        close(fd_novo);
    } else {
        int slot_livre = -1;
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (vizinhos[i].fd == -1) { slot_livre = i; break; }
        }
        
        if (slot_livre != -1) {
            vizinhos[slot_livre].fd = fd_novo;
            strcpy(vizinhos[slot_livre].id, res_id);
            strcpy(vizinhos[slot_livre].ip, res_ip);
            vizinhos[slot_livre].tcp = res_tcp;
            
            printf("[TCP] Ligado com sucesso ao Nó %s!\n", res_id);
            
            char neighbor_msg[128];
            sprintf(neighbor_msg, "NEIGHBOR %s\n", meu_id);
            write(fd_novo, neighbor_msg, strlen(neighbor_msg));
        } else {
            printf("[TCP] Estamos cheios! Impossível adicionar vizinho.\n");
            close(fd_novo);
        }
    }
}

void inicializar_rotas(struct Rota tabela_rotas[]) {
    for (int i = 0; i < 100; i++) {
        tabela_rotas[i].dist = -1;         
        strcpy(tabela_rotas[i].succ, "?"); 
        tabela_rotas[i].state = 0;         
        strcpy(tabela_rotas[i].succ_coord, "?");
        for(int j = 0; j < MAX_NEIGHBORS; j++) {
            tabela_rotas[i].coord[j] = 0;
        }
    }
}
