📌 INSTRUCCIONES PARA EJECUTAR EL COLLECTOR (Misael)


1. Instalar dependencias en Linux (Ubuntu o WSL o AWS)
    sudo apt update
    sudo apt install build-essential -y

2. Descargar mi archivo collector.c

Pueden ponerlo en cualquier carpeta, por ejemplo:

~/proyecto/collector.c

3. Compilar

En la terminal:

gcc -std=c11 -Wall -Wextra -pthread -o collector collector.c


Se debe generar un archivo ejecutable llamado:

collector

4. Ejecutar el servidor
./collector 9000


Eso dejará al colector escuchando en el puerto 9000
y mostrando la tabla actualizada cada 2 segundos.


3. ¿Qué se debe hacer para que sus agentes se conecten al servidor AWS?

    Nota: Mi servidor AWS está en la IP: 13.59.14.144 y el puerto es 9000
    Entonces, si ya tienen agent_mem o agent_cpu, lo ejecutan así:

    agent_mem
    
    ./agent_mem 13.59.14.144 9000 <ip_logica_del_agente>


    Ejemplo:

    ./agent_mem 13.59.14.144 9000 Nico-PC

    agent_cpu
    ./agent_cpu 13.59.14.144 9000 Tuli-PC

Apenas lo ejecuten, en su collector aparece su host/IP y los valores.

