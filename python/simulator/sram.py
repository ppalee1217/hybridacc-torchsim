

class SRAM:
    def __init__(self,
                 data_width,
                 address_bits,
                 read_latency=1,
                 write_latency=1):
        """
        Initialize the SRAM with given parameters.
        Args:
            data_width (int): Width of the data bus.
            address_bits (int): Number of bits for addressing.
            read_latency (int): Latency for read operations.
            write_latency (int): Latency for write operations.
        """
        self.data_width = data_width
        self.address_bits = address_bits
        self.memory = {}
        self.size = 2 ** address_bits
        self.read_latency = read_latency
        self.write_latency = write_latency

    def read(self, address):
        """
        Read data from the SRAM at the specified address.
        Args:
            address (int): Address to read from.
        Returns:
            data (int): Data read from the SRAM.
        """
        if address < 0 or address >= self.size:
            raise ValueError("Address out of bounds")

        return self.memory.get(address, 0)

    def write(self, address, data):
        """
        Write data to the SRAM at the specified address.
        Args:
            address (int): Address to write to.
            data (int): Data to write.
        """
        if address < 0 or address >= self.size:
            raise ValueError("Address out of bounds")
        if not (0 <= data < 2 ** self.data_width):
            # warning and truncate data if it exceeds the width
            print(f"Warning: Data {data} exceeds data width {self.data_width}, truncating.")
            data = data & ((1 << self.data_width) - 1)  # Truncate to fit data width

        self.memory[address] = data
        return True