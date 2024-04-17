#include <stdlib.h>
#include <stdio.h>
#include <utils/hello.h>
#include <pthread.h>
#include "main.h"

config_struct config;

int main(int argc, char* argv[]) {
    
    // creo hilos
    pthread_t thread_dispatch, thread_interrupt;

    int conexion_memoria;
    t_config* archivo_config = iniciar_config("cpu.config");    
    logger = log_create("cpu.log", "CPU", 1, LOG_LEVEL_DEBUG);

    cargar_config_struct_CPU(archivo_config);

    decir_hola("CPU");

    //Conexion con Memoria
    conexion_memoria = crear_conexion(config.ip_memoria, config.puerto_memoria);
    enviar_conexion("CPU", conexion_memoria);

    // Servidor CPU
    // conexion dispatch
    int socket_servidor_dispatch = iniciar_servidor(config.puerto_escucha_dispatch);
    log_info(logger, config.puerto_escucha_dispatch);
    log_info(logger, "Server CPU DISPATCH");

    // conexion interrupt
    int socket_servidor_interrupt = iniciar_servidor(config.puerto_escucha_interrupt);
    log_info(logger, config.puerto_escucha_interrupt);
    log_info(logger, "Server CPU INTERRUPT"); 

    // creo hilos para servidores CPU
    pthread_create(&thread_dispatch, NULL, servidor_escucha, &socket_servidor_dispatch);
    pthread_create(&thread_interrupt, NULL, servidor_escucha, &socket_servidor_interrupt);

    // espero a los que los hilos terminen su ejecución
    pthread_join(thread_dispatch, NULL);
    pthread_join(thread_interrupt, NULL);

    /*
    int cliente_dispatch = esperar_cliente(socket_servidor_dispatch);
    int continuar = 1;
    while(continuar){
        int cod_op = recibir_operacion(cliente_dispatch);
        switch (cod_op)
        {
        case CONEXION:
            recibir_conexion(cliente_dispatch);
            break;
        case -1:
            log_error(logger, "cliente desconectado de DISPATCH");
            continuar = 0;
            break;
        default:
            log_warning(logger, "Operacion desconocida.");
            break;
        }
    }


    int cliente_interrupt = esperar_cliente(socket_servidor_interrupt);
    continuar = 1;
    while(continuar){
        int cod_op = recibir_operacion(cliente_interrupt);
        switch (cod_op)
        {
        case CONEXION:
            recibir_conexion(cliente_interrupt);
            break;
        case -1:
            log_error(logger, "cliente desconectado de INT");
            continuar = 0;
            break;
        default:
            log_warning(logger, "Operacion desconocida.");
            break;
        }
    }
    */

    //Limpieza
    log_destroy(logger);
	config_destroy(archivo_config);
    liberar_conexion(conexion_memoria);

    return 0;
}

void cargar_config_struct_CPU(t_config* archivo_config){
    config.ip_memoria = config_get_string_value(archivo_config, "IP_MEMORIA");
    config.puerto_memoria = config_get_string_value(archivo_config, "PUERTO_MEMORIA");
    config.puerto_escucha_dispatch = config_get_string_value(archivo_config, "PUERTO_ESCUCHA_DISPATCH");
    config.puerto_escucha_interrupt = config_get_string_value(archivo_config, "PUERTO_ESCUCHA_INTERRUPT");
    config.cantidad_entradas_tlb = config_get_string_value(archivo_config, "CANTIDAD_ENTRADAS_TLB");
    config.algoritmo_tlb = config_get_string_value(archivo_config, "ALGORITMO_TLB");
}



