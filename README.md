# operating-systems_WAT
A semester project for operating systems

Description:
 * This program implements a robust Producer-Consumer pipeline using multiple processes, shared memory, message queues, and semaphores for synchronization. It features comprehensive signal handling to allow pausing, resuming, and terminating the data processing flow gracefully. Users can interact with the program through a menu-driven interface to select different input modes or exit the program.
   
 
Functionality:
- **Menu Options:**
 *   1. **User Input (Keyboard):** Reads data directly from the keyboard.
 *   2. **Input from File:** Reads data from a user-specified file.
 *   3. **/dev/urandom:** Continuously reads 1KB of random data from `/dev/urandom`.
 *   4. **Quit:** Exits the program.
 
- **Signal Handling:**
 *   - **SIGUSR1 (Termination):** Gracefully terminates the current processing pipeline and returns to the main menu.
 *   - **SIGUSR2 (Pause/Resume):** Toggles the pause and resume state of the data processing across all child processes.
 
- **Process Pipeline:**
 *   - **Process1 (Producer):** Reads data from the selected input source and writes it to shared memory.
 *   - **Process2 (Consumer→Producer):** Reads data from shared memory, converts it to hexadecimal format, and sends it to a message queue.
 *   - **Process3 (Consumer):** Retrieves hexadecimal data from the message queue and prints it to `stderr` in formatted chunks.
 
- **Inter-Process Communication (IPC):**
 *   - **Shared Memory:** Facilitates data transfer between Process1 and Process2.
 *   - **Semaphores:** Synchronize access to shared memory to prevent race conditions.
 *   - **Message Queues:** Enables communication between Process2 and Process3.
 
 * - **Graceful Termination and Cleanup:** Ensures all resources such as shared memory segments, semaphores, and message queues are properly cleaned up upon termination.
