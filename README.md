# Sistema de Monitoreo Distribuido (Collector + Agentes CPU/MEM)

👥 Integrantes

Misael Jesús Florez Anave – Implementación del Collector y despliegue en AWS EC2
Tuli Peña Melo – Implementación del agente de Memoria (agent_mem)
Andrew Nicolay Prieto Mendoza – Implementación del agente de CPU (agent_cpu)


📌 1. Descripción del Proyecto

Este sistema implementa un monitoreo distribuido usando programación en C y sockets TCP.
Consta de:

✔ Collector (servidor)

Corre en una máquina remota (AWS EC2)

Recibe conexiones simultáneas de múltiples agentes

Lee líneas con formatos:

MEM;ip;memUsed;memFree;swapTotal;swapFree
CPU;ip;cpuUsage;userPct;sysPct;idlePct


Mantiene una tabla de la última información por IP

Muestra un dashboard actualizado cada 2 segundos

✔ Agents

Cada agente corre en un cliente (PC local o remoto):

1) agent_mem

Lee /proc/meminfo y envía periódicamente:

MEM;<ip_logica>;memUsed;memFree;swapTotal;swapFree

2) agent_cpu

Lee /proc/stat y envía periódicamente:

CPU;<ip_logica>;cpuPct;userPct;sysPct;idlePct

📌 2. Estructura del repositorio
parcial_2/
│
├── collector.c
├── agent_cpu.c
├── agent_mem.c
├── README.md   ← este archivo

📌 3. ¿Cómo compilar cada componente?

Todos los binarios requieren gcc y pthreads.

✔ Compilar Collector
gcc -std=c11 -Wall -Wextra -pthread -o collector collector.c

✔ Compilar agente de memoria
gcc -std=c11 -Wall -Wextra -o agent_mem agent_mem.c

✔ Compilar agente de CPU
gcc -std=c11 -Wall -Wextra -o agent_cpu agent_cpu.c

📌 4. Despliegue en AWS EC2 (Collector)

Estos pasos solo deben hacerse una vez.

4.1 Crear una instancia EC2 (t2.micro o t3.micro Free Tier)

En AWS EC2 → Launch Instance:

Nombre: collector-server

AMI: Ubuntu 22.04 LTS

Tipo: t2.micro o t3.micro

Key Pair: Crear una llame awskey.pem

Reglas de firewall:

SSH (22) → 0.0.0.0/0
Custom TCP (9000) → 0.0.0.0/0

4.2 Conectarse desde Git Bash o WSL

Mover llave a ~/.ssh:

mkdir -p ~/.ssh
cp /mnt/c/Users/<TU_USUARIO>/Downloads/awskey.pem ~/.ssh/awskey.pem
chmod 400 ~/.ssh/awskey.pem


Conectarse:

ssh -i ~/.ssh/awskey.pem ubuntu@<PUBLIC_IP>


Ejemplo:

ssh -i ~/.ssh/awskey.pem ubuntu@13.59.14.144

4.3 Subir el archivo collector.c al servidor

Desde tu carpeta del proyecto:

scp -i ~/.ssh/awskey.pem collector.c ubuntu@<PUBLIC_IP>:/home/ubuntu/

4.4 Compilarlo en AWS:
gcc -std=c11 -Wall -Wextra -pthread -o collector collector.c

4.5 Ejecutar el collector
./collector 9000


Si todo está bien, verás:

IP       CPU   usr   sys   idle   MemUsed  MemFree
--------------------------------------------------

📌 5. Ejecutar los agentes (CPU y MEM)

Cada agente debe ser compilado en la máquina de cada estudiante.

5.1 En la PC del estudiante:
./agent_mem 13.59.14.144 9000 MiPC-MEM
./agent_cpu 13.59.14.144 9000 MiPC-CPU


Formato:

./agent_mem <ip_AWS> <puerto> <nombre_logico>
./agent_cpu <ip_AWS> <puerto> <nombre_logico>

Ejemplo real:
./agent_mem 13.59.14.144 9000 Nico-PC
./agent_cpu 13.59.14.144 9000 Tuli-PC

📌 6. ¿Qué debe ver el collector al recibir agentes?

Ejemplo:

IP        CPU usr sys idle   MemUsed MemFree
----------------------------------------------
MiPC-MEM  --   --  --   --    536.6   7263.8
MiPC-CPU  0.0  0.0 0.0 100.0  --      --


Cada 2 segundos se actualiza con los últimos datos enviados.

📌 7. Recomendaciones para el equipo
✔ Para que todo funcione:

Cada integrante compila su agente en su propia máquina

Todos usan la misma IP pública del AWS

El puerto debe ser 9000

El collector debe estar corriendo en AWS antes de lanzar agentes

✔ Qué enviar a cada integrante (Nico y Tuli)

Este README

IP pública de AWS

Los binarios o códigos de agentes

📌 8. Cierre y conclusiones

Este proyecto implementa:

Concurrencia mediante pthread
Conexiones TCP múltiples
Lectura de /proc en Linux
Parsing y actualización de estructuras compartidas
Visualización periódica de datos
Despliegue real en un servidor remoto (AWS EC2)

Demuestra dominio de:

Programación de bajo nivel en C
Sockets y redes
Sincronización
Infraestructura en la nube

📌 9. Comando resumen (TL;DR)
AWS
ssh -i ~/.ssh/awskey.pem ubuntu@<IP>
gcc -pthread -o collector collector.c
./collector 9000

Cliente
gcc -o agent_mem agent_mem.c
gcc -o agent_cpu agent_cpu.c

./agent_mem <IP_AWS> 9000 <nombre>
./agent_cpu <IP_AWS> 9000 <nombre>
