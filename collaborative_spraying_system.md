# Collaborative Swarm Spraying System: Implementation Guide

Welcome to the **Multi-Rover Collaborative Swarm Spraying System**! This guide is designed to help anyone—from beginners to advanced makers—understand how this decentralized robotic swarm works, how the rovers communicate, and how to build one.

---

## 1. What is a Collaborative Swarm System?

In standard setups, a single rover drives around a field to spray crops. If you add more rovers, they might drive over the same spots, waste water/chemicals, or crash into each other.

This **Swarm System** enables multiple rovers to operate in the same field safely and efficiently by communicating directly with one another.

### Key Capabilities:
*   **Decentralized Communication:** Rovers talk peer-to-peer using **ESP-NOW**. They do not need a Wi-Fi router or internet connection.
*   **Overlapping Spray Zone Avoidance:** If Rover 1 sprays a grid cell, it tells the group. Rover 2 marks that cell as "already sprayed" and will turn off its pump automatically if it passes through that spot.
*   **Collision Avoidance:** If two rovers get too close, the rover with the higher ID (lower priority) stops automatically and waits for the other to pass.
*   **Live Swarm Dashboard:** You can load the control webpage of any rover and see a live grid map showing where all rovers are and what parts of the field have been sprayed.

---

## 2. System Hardware Diagram

The diagram below shows how each individual rover's hardware modules connect to the ESP32-S3 microcontroller:

```text
                               ESP32-S3 Microcontroller
                           +------------------------------+
                           |                              |
      [L298N IN1] <--------+ GPIO 6                       |
      [L298N IN2] <--------+ GPIO 7                       |
      [L298N IN3] <--------+ GPIO 8   (Motor Controls)    |
      [L298N IN4] <--------+ GPIO 9                       |
      [L298N ENA] <--------+ GPIO 14                      |
      [L298N ENB] <--------+ GPIO 12                      |
                           |                              |
      [SERVO UP1] <--------+ GPIO 18                      |
      [SERVO UP2] <--------+ GPIO 13  (Pan/Tilt Servos)   |
     [SERVO LEFT] <--------+ GPIO 21                      |
                           |                              |
     [PUMP RELY1] <--------+ GPIO 2   (Pump Switches)     |
     [PUMP RELY2] <--------+ GPIO 15                      |
                           |                              |
      [TRIG SENS] <--------+ GPIO 5   (Ultrasonic Sensor) |
      [ECHO SENS] <--------+ GPIO 4                       |
                           +------------------------------+
```

---

## 3. How the Algorithms Work (Simplified)

### A. Communication via ESP-NOW
Rovers use **ESP-NOW**, a fast, low-power protocol built into the ESP32-S3. 
* Every **300ms**, each rover broadcasts a packet of data to a special broadcast address (`FF:FF:FF:FF:FF:FF`). 
* Every other rover within radio range hears this broadcast.
* The message contains:
  * **Who I am:** Rover ID (1, 2, 3...)
  * **Where I am:** Coordinate $(X, Y)$ on a $10 \times 10$ grid
  * **What I am doing:** Am I spraying? Which direction am I facing?

---

### B. Overlapping Spray Avoidance
1. The field is treated as a $10 \times 10$ grid map.
2. When Rover 1 turns on its spray pump, it records that its current grid cell is sprayed.
3. It broadcasts this to the swarm.
4. Rover 2 receives the message and marks that cell as `sprayed` in its local memory.
5. If you manually drive Rover 2 or if it autonomously enters that cell, the code checks the shared map. If the cell is already sprayed, the pump is blocked from turning on, saving your chemicals.

---

### C. Collision Avoidance (Priority Yielding)
Rovers check if their paths are about to cross:
1. Every time a rover receives a location packet from another rover, it calculates the distance between them.
2. If the distance is less than **1.5 grid units** (about 1.5 meters), a collision warning is triggered.
3. To resolve this without a central master, they use a **Priority ID Rule**:
   * **Rover 1** has the highest priority. **Rover 2** yields to Rover 1. **Rover 3** yields to Rover 1 and 2.
   * The yielding rover immediately stops its motors (`stopCar()`) and waits.
   * Once the higher-priority rover drives away and the distance is safe again, the yielding rover resumes driving.

---

### D. Position Estimation (Dead Reckoning)
Since outdoor fields may not have GPS or tracking cameras, the rovers estimate their own positions:
* The code starts the rover at a default grid coordinate of `(5, 5)` facing **North**.
* When you drive **Forward** for 1 second, the rover estimates it has moved 1 grid unit forward and updates its position to `(5, 6)`.
* When you turn **Right**, it rotates its virtual heading to **East**. Driving forward now changes the position to `(6, 6)`.

---

## 4. How to Configure and Setup your Swarm

Follow these steps to deploy the code to your rovers:

### Step 1: Change the Rover ID
Open the sketch in your Arduino IDE. Look at the top of the code for this line:
```cpp
#define ROVER_ID 1
```
* For your **first rover**, leave it as `1`. Upload the code.
* For your **second rover**, change it to `2`. Upload the code.
* For your **third rover**, change it to `3`. Upload the code.

### Step 2: Power Up
1. Power on all your rovers.
2. Connect your phone or laptop to the Wi-Fi hotspot (`Redmi Note 12`).
3. Open the Serial Monitor in the Arduino IDE for one of the rovers to see what IP address it received (e.g., `http://192.168.1.100`).

### Step 3: Open the Dashboard
1. Open your web browser and navigate to the printed IP address (e.g. `http://192.168.1.100`).
2. You will see a grid map on the screen:
   * **Red Block:** Your current rover.
   * **Blue Blocks:** Other active swarm rovers nearby.
   * **Green Blocks:** Areas that have been sprayed.
3. As you drive your rover, you will see it move on the grid in real-time. If you power on another rover and drive it, it will immediately appear on your screen as a blue peer block!
