#ifndef REDE_H
#define REDE_H

#define MAX_NEIGHBORS 10

struct Vizinho {
    int fd;
    char id[3];
    char ip[20];
    int tcp;
};

// Tabela de Encaminhamento - Preparada para o Protocolo Sem Ciclos
struct Rota {
    int dist;                
    char succ[3];            
    int state;               // 0 = Expedição, 1 = Coordenação
    char succ_coord[3];      // Quem causou a coordenação ("?" se foi quebra direta)
    int coord[MAX_NEIGHBORS];// Estado das respostas (1 = à espera, 0 = livre)
};

void inicializar_vizinhos(struct Vizinho vizinhos[]);
void mostrar_vizinhos(struct Vizinho vizinhos[]);
void remover_aresta(struct Vizinho vizinhos[], char *target_id);
void aceitar_ligacao(int fd_tcp_listen, struct Vizinho vizinhos[]);
void ligar_vizinho(struct Vizinho vizinhos[], char *res_id, char *res_ip, int res_tcp, char *meu_id);

void inicializar_rotas(struct Rota tabela_rotas[]);

#endif