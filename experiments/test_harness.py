### 2. Shared Test Harness (`experiments/cedar_testing.py`)
Save this to avoid repeating VM setup code.

```python
import cedar_core as cedar
import numpy as np

class CedarTestHost:
    """
    Manages the C++ VM for Python tests.
    Helps compile instructions and process audio blocks.
    """
    def __init__(self, sample_rate=48000):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.sr = sample_rate
        self.program = []
        self.param_counter = 0

    def load_instruction(self, instruction):
        """Append a raw instruction to the program."""
        self.program.append(instruction)

    def set_param(self, name: str, value: float) -> int:
        """
        Sets a VM parameter and returns the buffer index (10+)
        where the value can be read using ENV_GET.
        """
        self.vm.set_param(name, value)

        # Buffer 10-255 are free for params
        buf_idx = 10 + self.param_counter
        self.param_counter += 1

        # Instruction to fetch param into buffer
        name_hash = cedar.hash(name)
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, buf_idx, name_hash)
        )
        return buf_idx

    def process(self, input_signal: np.ndarray) -> np.ndarray:
        """
        Run the program on the input signal.
        """
        # Load program into VM
        self.vm.load_program(self.program)

        # Pad input to block boundary
        n_samples = len(input_signal)
        n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
        padded_len = n_blocks * cedar.BLOCK_SIZE

        input_padded = np.zeros(padded_len, dtype=np.float32)
        input_padded[:n_samples] = input_signal

        output_left = []

        # Process block by block
        for i in range(n_blocks):
            start = i * cedar.BLOCK_SIZE
            end = start + cedar.BLOCK_SIZE
            block_in = input_padded[start:end]

            # Inject input into Buffer 0 (standard input for these tests)
            self.vm.set_buffer(0, block_in)

            # Run VM
            l, r = self.vm.process()
            output_left.append(l)

        return np.concatenate(output_left)[:n_samples]