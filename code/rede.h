#ifndef REDE_H
#define REDE_H

/* ============================================================
 * MÓDULO DE REDE — rede.h
 * Projeto OWR (OverlayWithRouting)
 *
 * Este ficheiro define as estruturas de dados e declara as
 * funções públicas do módulo de rede. É o "contrato" entre
 * este módulo e o resto do programa (main.c, etc.).
 * ============================================================ */


/* Número máximo de vizinhos que um nó pode ter em simultâneo.
 * Cada vizinho ocupa um slot no array e corresponde a uma
 * ligação TCP ativa. */
#define MAX_NEIGHBORS 10


/* ------------------------------------------------------------
 * struct Vizinho
 *
 * Representa um vizinho direto na rede de sobreposição,
 * ou seja, um nó com o qual existe uma ligação TCP ativa.
 *
 * O array de vizinhos tem MAX_NEIGHBORS slots. Um slot está
 * livre se fd == -1. Um slot está ocupado se fd >= 0.
 * ------------------------------------------------------------ */
struct Vizinho {
    int  fd;       /* Descritor do socket TCP para este vizinho.
                    * -1 = slot livre (sem vizinho).
                    * >= 0 = ligação TCP ativa. */

    char id[3];    /* Identificador do vizinho (2 dígitos + '\0').
                    * '?' = vizinho ainda não se apresentou via NEIGHBOR.
                    * Preenchido quando recebemos "NEIGHBOR <id>" ou
                    * imediatamente se fomos nós a iniciar a ligação. */

    char ip[20];   /* Endereço IP do vizinho (ex: "192.168.1.5").
                    * Preenchido apenas em ligações ativas (ligar_vizinho). */

    int  tcp;      /* Porto TCP do vizinho.
                    * Preenchido apenas em ligações ativas (ligar_vizinho). */
};


/* ------------------------------------------------------------
 * struct Rota
 *
 * Representa uma entrada na tabela de encaminhamento.
 * Existe uma entrada por cada possível destino (IDs 00–99),
 * portanto o array tem 100 entradas.
 *
 * O índice do array corresponde ao ID numérico do destino:
 *   tabela_rotas[15] = rota para o nó "15"
 *
 * Esta estrutura foi desenhada para suportar o protocolo de
 * encaminhamento SEM CICLOS, com dois estados possíveis:
 *   - EXPEDIÇÃO  (state=0): rota estável, encaminhamento normal.
 *   - COORDENAÇÃO (state=1): rota em renegociação, aguarda
 *     respostas dos vizinhos antes de atualizar.
 * ------------------------------------------------------------ */
struct Rota {
    int  dist;                  /* Distância em saltos até ao destino.
                                 * -1  = destino desconhecido / inacessível.
                                 * >= 0 = número de saltos até ao destino. */

    char succ[3];               /* Identificador do vizinho de expedição —
                                 * o próximo salto para alcançar o destino.
                                 * '?'  = sem rota definida.
                                 * '-1' = em coordenação, sem sucessor ativo. */

    int  state;                 /* Estado do protocolo sem ciclos:
                                 * 0 = EXPEDIÇÃO   — rota estável, pode encaminhar.
                                 * 1 = COORDENAÇÃO — a aguardar respostas dos vizinhos
                                 *     antes de comprometer uma nova rota. */

    char succ_coord[3];         /* ID do vizinho que desencadeou a entrada em
                                 * coordenação (quem enviou a mensagem COORD).
                                 * '?' = nenhuma coordenação ativa.
                                 * Usado para enviar UNCOORD ao vizinho correto
                                 * quando a coordenação termina. */

    int  coord[MAX_NEIGHBORS];  /* Array de flags de resposta, indexado pelo
                                 * slot do vizinho no array de vizinhos.
                                 * coord[i] = 1: ainda a aguardar resposta do
                                 *              vizinho no slot i (UNCOORD ou ROUTE).
                                 * coord[i] = 0: vizinho já respondeu ou não aplicável.
                                 * Quando todos forem 0, a coordenação termina. */
};


/* ------------------------------------------------------------
 * Declarações das funções públicas do módulo
 * ------------------------------------------------------------ */

/* Inicializa todos os slots do array como livres (fd = -1). */
void inicializar_vizinhos(struct Vizinho vizinhos[]);

/* Imprime no ecrã a lista de vizinhos atualmente conectados. */
void mostrar_vizinhos(struct Vizinho vizinhos[]);

/* Fecha a ligação TCP com o vizinho de ID target_id e
 * liberta o seu slot no array. */
void remover_aresta(struct Vizinho vizinhos[], char *target_id);

/* Aceita uma ligação TCP de entrada e guarda o novo fd num
 * slot livre. O id fica '?' até chegar mensagem NEIGHBOR. */
void aceitar_ligacao(int fd_tcp_listen, struct Vizinho vizinhos[]);

/* Estabelece ativamente uma ligação TCP ao nó (res_ip, res_tcp),
 * guarda os seus dados e envia imediatamente "NEIGHBOR meu_id". */
void ligar_vizinho(struct Vizinho vizinhos[], char *res_id,
                   char *res_ip, int res_tcp, char *meu_id);

/* Inicializa todas as entradas da tabela de rotas como
 * desconhecidas: dist=-1, succ='?', state=0 (expedição). */
void inicializar_rotas(struct Rota tabela_rotas[]);


#endif /* REDE_H */
