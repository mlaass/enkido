# **Project NKIDO: Technical Specification & PRD**

## **1\. Vision**

A lightweight, high-performance audio synthesis language designed for two primary use cases:

1. **Live Coding:** Real-time manipulation of musical structures with state preservation (hot-swapping).
2. **Embedded Audio:** A portable runtime for games and apps that executes pre-compiled bytecode with external parameter binding.

## **2\. System Architecture Overview**

The system is divided into three distinct layers:

* **Frontend (The Compiler):** Lexer, Recursive Descent Parser, and Semantic Analyzer.
* **Middle-end (The Optimizer):** Topological Sorter, Semantic ID Generator, and Bytecode Encoder.
* **Backend (The VM):** A register-based Virtual Machine processing audio in blocks with a persistent State Pool.

## **3\. Frontend: Recursive Descent Parser & AST**

The parser transforms source text into an Abstract Syntax Tree (AST) using an iterative approach to avoid stack overflows and handle operator precedence.

### **Key Requirements:**

* **Expression Handling:** Use **Precedence Climbing** or a **Pratt Parser** to handle mathematical operations ($+, \-, \*, /$) without left-recursion.
* **Lexer Optimization:** Implement **String Interning** using a hash map to convert keywords and identifiers into unique uint32\_t IDs, allowing the parser to perform integer comparisons instead of string comparisons.
* **Data-Oriented AST:** Instead of new Node() pointer chasing, store the AST in a contiguous std::vector\<Node\>.
* **Indices over Pointers:** Use uint32\_t indices for child/sibling links to reduce memory footprint and improve cache locality.
* **Semantic ID Tracking:** As the parser descends, it maintains a "Path Stack" (e.g., main/track1/osc). Use a fast non-cryptographic hash like **FNV-1a** to generate the stable stateId.

### **Code Sketch: Path Tracking**

void Parser::parseOscillator() {
    pushPath("osc"); // Add context to the semantic hash
    uint64\_t semanticId \= currentPathHash();

    expect(TOKEN\_OSC);
    expect(TOKEN\_LPAREN);
    parseExpression(); // Frequency
    expect(TOKEN\_RPAREN);

    // Store in Arena with its stable ID
    uint32\_t nodeIdx \= astArena.emplace\_back(NODE\_OSC, semanticId);
    popPath();
}

## **4\. Middle-end: DAG Construction & Optimization**

The AST is "flattened" into a Directed Acyclic Graph representing signal flow.

### **Topological Sort (Execution Ordering)**

The VM must execute nodes in a strict dependency order (e.g., an Oscillator must fill its buffer before a Filter processes it).

* **Algorithm:** Kahn’s Algorithm or DFS-based Topological Sort.
* **Result:** A linear array of instructions where all buffer dependencies are satisfied.

### **Structural Diffing & Hot-Swapping**

When the user updates code during live performance:

1. **Identity Matching:** Compare the Semantic IDs of the new DAG against the current running DAG.
2. **State Re-binding:** If an ID matches (e.g., \#lead\_osc), the new instruction is pointed to the existing phase/memory in the StatePool.
3. **Local Crossfading:** If a node is new or structural topology changes significantly, the VM performs a 5-10ms "micro-fade" on that specific branch to prevent DC-offset pops.

## **5\. Backend: The Bytecode Virtual Machine**

The VM is designed for cache optimality and zero-latency execution.

### **Memory Layout (Data-Oriented Design)**

* **Buffer Pool:** A pre-allocated arena of audio buffers (e.g., float\[128\]). These act as the "registers" for the bytecode.
* **State Pool:** A persistent storage area for "memory" nodes (oscillator phases, filter delay lines, envelope stages).
* **Instruction Set:** Fixed-width 128-bit instructions for fast decoding.
* **Branch Prediction:** Use C++20 \[\[likely\]\] and \[\[unlikely\]\] attributes in the VM's switch statement to hint the success path.

struct Instruction {
    uint8\_t opcode;      // OP\_SINE, OP\_MUL, OP\_LPF
    uint8\_t rate;        // Control-rate (k) vs Audio-rate (a)
    uint16\_t outBuffer;  // Index in Buffer Pool
    uint16\_t inputs\[3\];  // Input buffer indices
    uint64\_t stateId;    // Stable hash for State Pool lookup
};

## **6\. Live Features: State Preservation**

The "Magic" of NKIDO is the decoupling of the **Instruction** (the "What") from the **State** (the "Memory").

### **Persistent State Pool**

The StatePool keeps track of which nodes are "Alive."

* **Touch Tracking:** Every time a node is executed, its state is marked as "touched."
* **Garbage Collection:** After a hot-swap, states that haven't been touched for X blocks are faded out and deleted to free memory.

## **7\. Embedded Use Case: External Inputs**

To support games (Unreal/Godot) or MIDI controllers, the system uses **Environment Registers**.

* **Global Input Map:** A thread-safe, lock-free map where the host application writes values (e.g., SetParam("Speed", 0.8)).
* **Zipper Noise Mitigation:** The VM's OP\_GET\_ENV opcode automatically performs **linear interpolation** or exponential smoothing between audio blocks to prevent abrupt clicks.

## **8\. Technical Challenges & Mitigations**

| Challenge | Mitigation |
| :---- | :---- |
| **Audio Glitches (Pops)** | Implement 5ms micro-crossfades on structural graph changes. |
| **Thread Safety** | **Triple Buffer** approach: Compiler writes to "Next," Audio Thread reads from "Current." Atomic pointer swap at block boundaries. |
| **Cache Misses** | Use contiguous std::vector for all instructions. Process audio in "vectors" (blocks of 64/128) rather than sample-by-sample. |
| **Parallelization** | Use a task-based scheduler like **cpp-taskflow** to identify independent branches in the DAG and dispatch them to worker threads. |

## **9\. Implementation Roadmap**

1. **Phase 1:** Basic Lexer/Parser with Precedence Climbing, String Interning, and Arena-based AST.
2. **Phase 2:** Topological Sorter and a single-threaded Bytecode VM with Buffer Pooling.
3. **Phase 3:** Semantic ID Path Tracking (FNV-1a) and the persistent State Pool for hot-swapping.
4. **Phase 4:** External Input API (Environment Map) with Linear Interpolation for zipper-noise reduction.
5. **Phase 5:** Integration of **Taskflow** for multi-threaded DAG execution and SIMD (SSE/AVX) optimization for the VM hot-loop.