#include <stdlib.h>
#include <stdio.h>
#include <utils/hello.h>
#include "paginacion.h" 
#include "main.h"

pthread_t thread_memoria;

int main(int argc, char **argv){

    // ------ INICIALIZACIÓN VARIABLES GLOBALES ------ //
    t_config *archivo_config = iniciar_config(argv[1]);
    cargar_config_struct_MEMORIA(archivo_config);
    logger = log_create("memoria.log", "Servidor Memoria", 1, LOG_LEVEL_INFO);
    decir_hola("Memoria");
    sem_init(&mutex_espacio_usuario, 0, 1); 
    sem_init(&mutex_bitmap_marcos, 0, 1); 
    sem_init(&mutex_tablas_paginas_global, 0, 1); 
    init_memoria(); // inicializa la void* memoria y las estructuras necesarias para la paginacion

    // ------ INICIALIZACIÓN SERVIDOR + HILO ESCUCHA ------ //
    int socket_servidor = iniciar_servidor(config.puerto_escucha);
    log_info(logger, "puerto %s", config.puerto_escucha);
    log_info(logger, "Server MEMORIA iniciado");

    servidor_escucha(&socket_servidor);

    sem_destroy(&mutex_bitmap_marcos);
    sem_destroy(&mutex_espacio_usuario);
    sem_destroy(&mutex_tablas_paginas_global);
    log_destroy(logger);
    config_destroy(archivo_config);
    bitarray_destroy(bitmap_marcos);
    // TODO: FREE SEMAFORO

    return EXIT_SUCCESS;
}

void cargar_config_struct_MEMORIA(t_config *archivo_config){
    config.puerto_escucha = config_get_string_value(archivo_config, "PUERTO_ESCUCHA");
    config.tam_memoria = config_get_int_value(archivo_config, "TAM_MEMORIA");
    config.tam_pagina = config_get_int_value(archivo_config, "TAM_PAGINA");
    config.path_instrucciones = config_get_string_value(archivo_config, "PATH_INSTRUCCIONES");
    config.retardo_respuesta = config_get_int_value(archivo_config, "RETARDO_RESPUESTA");
}

void *atender_cliente(void *cliente){
    int cliente_recibido = *(int *)cliente;
    t_temporal* timer; // cada cliente tendrá un timer asociado para controlar el retardo de las peticiones
    while (1){
        int cod_op = recibir_operacion(cliente_recibido); // bloqueante
        switch (cod_op){
        case CONEXION:
            recibir_conexion(cliente_recibido);
        break;
        case DATOS_MEMORIA:
            t_sbuffer* buffer_a_cpu_datos_memoria = buffer_create(
                sizeof(int) * 2
            );
            buffer_add_int(buffer_a_cpu_datos_memoria, config.tam_memoria);
            buffer_add_int(buffer_a_cpu_datos_memoria, config.tam_pagina);
            cargar_paquete(cliente_recibido, DATOS_MEMORIA, buffer_a_cpu_datos_memoria);
        break;
        case INICIAR_PROCESO: // KERNEL
            timer = temporal_create(); // SIEMPRE PRIMERO ESTO! calcula tiempo de petición
            t_sbuffer *buffer_path = cargar_buffer(cliente_recibido);
            uint32_t pid_iniciar = buffer_read_uint32(buffer_path);
            uint32_t longitud_path;
            char* path  = buffer_read_string(buffer_path, &longitud_path);
            path[strcspn(path, "\n")] = '\0'; // CORREGIR: DEBE SER UN PROBLEMA DESDE EL ENVÍO DEL BUFFER!

            crear_proceso(pid_iniciar, path, longitud_path); 

            op_code respuesta_kernel = CONTINUAR; // si todo fue ok enviar CONTINUAR para que kernel pueda continuar con su planificacion
            tiempo_espera_retardo(timer); // ANTES DE RESPONDER ESPERO QUE FINALICE EL RETARDO
            send(cliente_recibido, &respuesta_kernel, sizeof(respuesta_kernel), 0);

            free(path);
            buffer_destroy(buffer_path);
        break;
        case ELIMINAR_PROCESO: // KERNEL
            timer = temporal_create();
            t_sbuffer* buffer_eliminar = cargar_buffer(cliente_recibido);
            uint32_t pid_eliminar = buffer_read_uint32(buffer_eliminar);

            log_debug(logger, "se mando a eliminar/liberar proceso %u", pid_eliminar);
            eliminar_proceso(pid_eliminar); 

            op_code respuesta_cpu = CONTINUAR;
            tiempo_espera_retardo(timer);
            send(cliente_recibido, &respuesta_cpu, sizeof(respuesta_cpu), 0);

            buffer_destroy(buffer_eliminar);
        break;
        case LEER_PROCESO: // CPU
            // 1. comienza a contar el timer para calcular retardo en rta a CPU
            timer = temporal_create();

            // 2. cargo buffer
            t_sbuffer *buffer_lectura_instrucciones = cargar_buffer(cliente_recibido);
            uint32_t pid_proceso = buffer_read_uint32(buffer_lectura_instrucciones); // guardo PID del proceso del cual se quiere leer
            uint32_t pc_proceso = buffer_read_uint32(buffer_lectura_instrucciones);  // guardo PC para elegir la prox INSTRUCCION a ejecutar

            log_debug(logger, "Recibi para leer del proceso %u, la prox INST desde PC %u", pid_proceso, pc_proceso);

            t_pcb* proceso_memoria = get_element_from_pid(pid_proceso);
            if(!proceso_memoria) {
                log_error(logger, "no existe dicho proceso en el pcb de memoria");
                break;
            }
            char* path_instrucciones_proceso = malloc(strlen(config.path_instrucciones) + strlen(proceso_memoria->path_proceso) + 1);
            strcpy(path_instrucciones_proceso, config.path_instrucciones);
            strcat(path_instrucciones_proceso, proceso_memoria->path_proceso);

            log_debug(logger, "este es el path absoluto del proceso %s", path_instrucciones_proceso);
            int lineaActual = 0, lee_instruccion = 0;

            char *instruccion = NULL;
            size_t len = 0;
            FILE *script = fopen(path_instrucciones_proceso, "r");

            if (script == NULL){
                log_error(logger, "No se encontro ningun archivo con el nombre indicado...");
                temporal_destroy(timer);
            } else {
                while (getline(&instruccion, &len, script) != -1){
                    if (lineaActual == pc_proceso){
                        log_debug(logger, "INSTRUCCION LEIDA EN LINEA %d: %s", pc_proceso, instruccion);
                        lee_instruccion = 1;
                        break;
                    } else {
                        free(instruccion);
                        instruccion = NULL;
                        lineaActual++;
                    }
                }
                fclose(script);
                
                if(lee_instruccion){
                    t_sbuffer *buffer_instruccion = buffer_create(
                        (uint32_t)strlen(instruccion) + sizeof(uint32_t)
                    );

                    buffer_add_string(buffer_instruccion, (uint32_t)strlen(instruccion), instruccion);

                    tiempo_espera_retardo(timer); // SIEMPRE, antes de terminar la petición a memoria, espera a que se complete el retardo de la configuración
                    cargar_paquete(cliente_recibido, INSTRUCCION, buffer_instruccion);
                    buffer_destroy(buffer_lectura_instrucciones);
                    free(instruccion);
                } // else TODO: llega al final y CPU todavía no desalojó el proceso por EXIT (esto pasaría sólo si el proceso no termina con EXIT)
            }
            free(path_instrucciones_proceso);
        break;
        case RESIZE: // solo CPU
            timer = temporal_create(); // SIEMPRE PRIMERO ESTO! calcula tiempo de petición
            t_sbuffer *buffer_resize = cargar_buffer(cliente_recibido);
            uint32_t proceso_resize = buffer_read_uint32(buffer_resize);
            int tamanio_resize = buffer_read_int(buffer_resize);
            resize_proceso(timer, cliente_recibido, proceso_resize, tamanio_resize); // ya responde a CPU y se encarga terminar el retardo
            buffer_destroy(buffer_resize);
        break;
        case TLB_MISS: // CPU
            timer = temporal_create();
            t_sbuffer* buffer_tlb_miss = cargar_buffer(cliente_recibido);
            uint32_t proceso_tlb_miss = buffer_read_uint32(buffer_tlb_miss);
            int pagina = buffer_read_int(buffer_tlb_miss);

            uint32_t marco = obtener_marco_proceso(proceso_tlb_miss, pagina);

            t_sbuffer* buffer_marco_solicitado = buffer_create(sizeof(uint32_t));
            buffer_add_uint32(buffer_marco_solicitado, marco);
            tiempo_espera_retardo(timer);
            cargar_paquete(cliente_recibido, MARCO_SOLICITADO, buffer_marco_solicitado);

            buffer_destroy(buffer_tlb_miss);
        break;
        case PETICION_ESCRITURA: // CPU / IO
            t_sbuffer* buffer_escritura = cargar_buffer(cliente_recibido);
            uint32_t proceso_peticion_escritura = buffer_read_uint32(buffer_escritura);
            int cantidad_peticiones_escritura = buffer_read_int(buffer_escritura);

            for (int i = 0; i < cantidad_peticiones_escritura; i++){
                timer = temporal_create(); // por cada peticion de escritura corre el TIMER

                uint32_t direccion_fisica = buffer_read_uint32(buffer_escritura);
                uint32_t bytes_peticion;
                void* dato_escritura = buffer_read_void(buffer_escritura, &bytes_peticion);

                log_info(logger, "PID: %u - Accion: ESCRIBIR - Direccion fisica: %u - Tamaño %u", proceso_peticion_escritura, direccion_fisica, bytes_peticion);
                escribir_memoria(direccion_fisica, dato_escritura, bytes_peticion);

                free(dato_escritura); // va liberando memoria ya escrita!
                tiempo_espera_retardo(timer);
            }

            op_code respuesta_escritura = CONTINUAR;
            send(cliente_recibido, &respuesta_escritura, sizeof(respuesta_escritura), 0);
            buffer_destroy(buffer_escritura);          
        break;
        case PETICION_LECTURA: // CPU / IO
            t_sbuffer* buffer_lectura = cargar_buffer(cliente_recibido);
            uint32_t proceso_peticion_lectura = buffer_read_uint32(buffer_lectura);
            int cantidad_peticiones_lectura = buffer_read_int(buffer_lectura);

            uint32_t tamanio_dato_lectura = calcular_tamanio_dato_lectura(buffer_lectura, cantidad_peticiones_lectura);
            t_sbuffer* buffer_datos_lectura = buffer_create(
                    sizeof(int) + // cantidad_peticiones_lectura
                    sizeof(uint32_t) * cantidad_peticiones_lectura + // uint32_t de cada void* a leer
                    tamanio_dato_lectura // cantidad de bytes totales del dato a leer (que podría estar partido en varios void*)
            );
            buffer_add_int(buffer_datos_lectura, cantidad_peticiones_lectura);

            for (int i = 0; i < cantidad_peticiones_lectura; i++){
                timer = temporal_create(); // por cada peticion de escritura corre el TIMER

                uint32_t direccion_fisica = buffer_read_uint32(buffer_lectura);
                uint32_t bytes_peticion = buffer_read_uint32(buffer_lectura);

                log_info(logger, "PID: %u - Accion: LEER - Direccion fisica: %u - Tamaño %u", proceso_peticion_lectura, direccion_fisica, bytes_peticion);
                void* dato_leido = leer_memoria(direccion_fisica, bytes_peticion);
                buffer_add_void(buffer_datos_lectura, dato_leido, bytes_peticion);
                free(dato_leido); // va liberando memoria ya cargada en buffer!

                tiempo_espera_retardo(timer);
            }

            cargar_paquete(cliente_recibido, PETICION_LECTURA, buffer_datos_lectura);
            buffer_destroy(buffer_lectura); 
        break;
        case -1:
            log_error(logger, "Cliente desconectado.");
            close(cliente_recibido); // cierro el socket accept del cliente
            free(cliente);           // libero el malloc reservado para el cliente
            pthread_exit(NULL);      // solo sale del hilo actual => deja de ejecutar la función atender_cliente que lo llamó
        break;
        default:
            log_warning(logger, "Operacion desconocida.");
        break;
        }
    }
}


////// funciones memoria
// RESIZE

void resize_proceso(t_temporal* timer, int socket_cliente, uint32_t pid, int new_size){

    // 1. Primera validación: el resize NO es mayor al tam_memoria => si fuese mayor se estaría pidiendo una cantidad de páginas MAYOR a la que puede tener cada tabla de páginas
    if(!suficiente_memoria(new_size)){
        op_code respuesta_cpu = OUT_OF_MEMORY;
        tiempo_espera_retardo(timer);
        ssize_t bytes_enviados = send(socket_cliente, &respuesta_cpu, sizeof(respuesta_cpu), 0);
        if (bytes_enviados == -1) {
            log_error(logger, "Error enviando dato OUT_OF_MEMORY a CPU");
            exit(EXIT_FAILURE);
        }
        return;
    }
    
    // 2. Analizar si corresponde ampliar o reducir el proceso
    int cantidad_paginas_ocupadas = cant_paginas_ocupadas_proceso(pid); // paginas ocupadas por el proceso
    int cantidad_paginas_solicitadas = (int)ceil((double)new_size / config.tam_pagina); // paginas que se necesitan con el nuevo valor del resize
    
    if (cantidad_paginas_solicitadas > cantidad_paginas_ocupadas){
        int paginas_a_aumentar = cantidad_paginas_solicitadas - cantidad_paginas_ocupadas;
        // 3. Segunda validación: ¿hay suficientes marcos libres?
        uint32_t* marcos_solicitados = buscar_marcos_libres(paginas_a_aumentar); // TODO: AGREGAR SINCRONIZACION EN LA FUNCIÓN
        if(!marcos_solicitados){
            op_code respuesta_cpu = OUT_OF_MEMORY;
            tiempo_espera_retardo(timer);
            ssize_t bytes_enviados = send(socket_cliente, &respuesta_cpu, sizeof(respuesta_cpu), 0);
            if (bytes_enviados == -1) {
                log_error(logger, "Error enviando dato OUT_OF_MEMORY a CPU");
                exit(EXIT_FAILURE);
            }
            return;
        }
        log_info(logger, "PID: %u - Tamaño Actual: %d - Tamaño a Ampliar: %d", pid, cantidad_paginas_ocupadas*config.tam_pagina, cantidad_paginas_solicitadas*config.tam_pagina);
        ampliar_proceso(pid, paginas_a_aumentar, marcos_solicitados);
    } else if (cantidad_paginas_solicitadas < cantidad_paginas_ocupadas){
        int paginas_a_reducir = cantidad_paginas_ocupadas - cantidad_paginas_solicitadas;
        log_info(logger, "PID: %u - Tamaño Actual: %d - Tamaño a Reducir: %d", pid, cantidad_paginas_ocupadas*config.tam_pagina, cantidad_paginas_solicitadas*config.tam_pagina);
        reducir_proceso(pid, paginas_a_reducir);
    } // si cantidad_paginas_solicitadas == cantidad_paginas_ocupadas NO se hace nada
    op_code respuesta_cpu = CONTINUAR;
    tiempo_espera_retardo(timer);
    send(socket_cliente, &respuesta_cpu, sizeof(respuesta_cpu), 0);
    return;
}

// AMPLIACION 
void ampliar_proceso(uint32_t pid, int cantidad, uint32_t* marcos_solicitados){
    t_pcb* proceso = get_element_from_pid(pid);
    for(int i = 0; i < cantidad; i++){
        create_pagina(proceso->tabla_paginas, marcos_solicitados[i]); 
        log_debug(logger, "cree una nueva pagina en la posicion %d de la tabla de pags. del proceso %u y le asigne el marco numero %u", list_size(proceso->tabla_paginas) - 1, proceso->pid, marcos_solicitados[i]);
    }
    free(marcos_solicitados);
}

void reducir_proceso(uint32_t pid, int cantidad){
    t_pcb* proceso = get_element_from_pid(pid);
    liberar_paginas(proceso, cantidad);
}

/*          RETARDO CONFIG          */
void tiempo_espera_retardo(t_temporal* timer) {
    // cuento ms del timer y espero a que se complete el tiempo de retardo!
    int64_t tiempo_transcurrido = temporal_gettime(timer);

    if (tiempo_transcurrido < config.retardo_respuesta) {
        usleep((config.retardo_respuesta - tiempo_transcurrido) * 1000); // usleep espera en microsegundos
    }
 
    temporal_destroy(timer);
}
    
    
/*          ACCESO ESPACIO USUARIO            */
uint32_t calcular_tamanio_dato_lectura(t_sbuffer* buffer, int cantidad_peticiones){
    uint32_t offset_original = buffer->offset, tamanio_buffer = 0;
    for (int i = 0; i < cantidad_peticiones; i++){
        buffer_read_uint32(buffer); // lee la dir. fisica pero acá no la trabaja (sólo se hace para avanzar el offset!!!)
        uint32_t bytes_peticion = buffer_read_uint32(buffer);
        tamanio_buffer += bytes_peticion;
    }
    buffer->offset = offset_original;
    return tamanio_buffer;    
}

// uint32_t marco*tam_pagina + offset
void* leer_memoria(uint32_t dir_fisica, uint32_t tam_lectura){
    void* dato = malloc(tam_lectura);
    // memcpy (donde guardo el dato (posicionado) / desde dónde saco el dato (posicionado) / el tamaño de lo que quiero sacar)
    sem_wait(&mutex_espacio_usuario);
    memcpy(dato, memoria + dir_fisica, tam_lectura);
    sem_post(&mutex_espacio_usuario);
    return dato;
}


int escribir_memoria(uint32_t dir_fisica, void* dato, uint32_t tam_escritura){
    // memcpy (donde guardo el dato (posicionado) / desde dónde saco el dato (posicionado) / el tamaño de lo que quiero sacar)
    sem_wait(&mutex_espacio_usuario);
    memcpy(memoria + dir_fisica, dato, tam_escritura);
    sem_post(&mutex_espacio_usuario);

    void* dato_escrito = leer_memoria(dir_fisica, tam_escritura);
    if(!dato_escrito){
        log_debug(logger, "parece que no se escribio bien");
        free(dato_escrito);
        return DESALOJAR;
    } else {
        log_debug(logger, "lo que acabamos de escribir ¿? %s .", (char*)dato_escrito);
        free(dato_escrito);
        return CONTINUAR;
    }
}


