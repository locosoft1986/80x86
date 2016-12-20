#include <gtest/gtest.h>
#include <VPrefetch.h>
#include <vector>
#include <map>

#include "VerilogTestbench.h"

class PrefetchTestFixture : public VerilogTestbench<VPrefetch>,
    public ::testing::Test {
public:
    PrefetchTestFixture();
    std::vector<uint8_t> fifo_bytes;
    std::map<uint32_t, uint16_t> memory;
    int mem_latency;
private:
    bool reading;
};

PrefetchTestFixture::PrefetchTestFixture()
    : mem_latency(0), reading(false)
{
    dut.reset = 0;
    dut.cs = 0;
    dut.new_ip = 0;
    dut.load_new_ip = 0;
    dut.fifo_full = 0;
    dut.mem_access = 0;
    dut.mem_ack = 0;
    dut.mem_data = 0;

    at_edge(PosEdge, [&]{
        if (!this->dut.reset && this->dut.fifo_wr_en)
            fifo_bytes.push_back(this->dut.fifo_wr_data & 0xff);
    });
    at_edge(NegEdge, [&]{
        this->dut.mem_ack = 0;
        this->dut.mem_data = 0;
        if (!this->dut.reset && this->dut.mem_access && !reading) {
            reading = true;
            if (memory.find(this->dut.mem_address) == memory.end())
                FAIL() << "no memory at 0x" << std::hex << this->dut.mem_address;
            after_n_cycles(mem_latency, [&]{
                this->dut.mem_data = memory[this->dut.mem_address];
                this->dut.mem_ack = 1;
                after_n_cycles(1, [&]{ this->dut.mem_ack = 0; reading = false; });
            });
        }
    });
    reset();
}

TEST_F(PrefetchTestFixture, full_fifo_no_fetch)
{
    dut.fifo_full = 1;
    cycle();
    ASSERT_FALSE(dut.mem_access);
}

TEST_F(PrefetchTestFixture, empty_fifo_triggers_fetch)
{
    memory[0] = 0;
    dut.fifo_full = 0;
    cycle();
}

TEST_F(PrefetchTestFixture, fetch_writes_to_fifo)
{
    memory[0] = 0xaa55;
    memory[2] = 0xdead;
    dut.fifo_full = 0;

    cycle(3);

    auto expected = std::vector<uint8_t>{0x55, 0xaa};
    ASSERT_EQ(fifo_bytes, expected);
}

TEST_F(PrefetchTestFixture, update_ip_resets_fifo)
{
    memory[0x000] = 0xf00f;
    memory[0x100] = 0x1234;
    memory[0x102] = 0x0ff0;
    dut.new_ip = 0x101;
    dut.load_new_ip = 1;
    cycle();
    dut.load_new_ip = 0;
    ASSERT_TRUE(dut.fifo_reset);
    ASSERT_EQ(dut.mem_address, 0x100LU);
    cycle(4);
    ASSERT_EQ(fifo_bytes, std::vector<uint8_t>{0x12});
}

TEST_F(PrefetchTestFixture, odd_fetch_address_writes_one)
{
    memory[0x000] = 0xbeef;
    memory[0x100] = 0x1234;
    memory[0x102] = 0xdead;
    dut.new_ip = 0x101;
    dut.load_new_ip = 1;
    cycle();
    dut.load_new_ip = 0;
    cycle(4);

    ASSERT_EQ(fifo_bytes, std::vector<uint8_t>{0x12});
}

TEST_F(PrefetchTestFixture, back_to_back_reads)
{
    memory[0x000] = 0xbeef;
    memory[0x100] = 0x12aa;
    memory[0x102] = 0x5634;
    memory[0x104] = 0xffff;
    dut.new_ip = 0x101;
    dut.load_new_ip = 1;
    cycle();
    dut.load_new_ip = 0;
    // Start fetching from the new address, this should do back to back reads,
    // so 6 cycles to write the 3 bytes.
    cycle(6);

    ASSERT_EQ(fifo_bytes, (std::vector<uint8_t>{0x12, 0x34, 0x56}));
}

TEST_F(PrefetchTestFixture, address_generation)
{
    memory[0xdead8] = 0xf00d;
    dut.cs = 0xdead;
    dut.new_ip = 0x8;
    dut.load_new_ip = 1;
    cycle();
    ASSERT_EQ(dut.mem_address, 0xdead8LU);
}

TEST_F(PrefetchTestFixture, new_ip_discards_current_fetch)
{
    mem_latency = 5;
    memory[0xdead0] = 0xffff;
    memory[0xdead8] = 0xf00d;
    memory[0xdeada] = 0xface;
    memory[0xdeadc] = 0x5555;
    dut.cs = 0xdead;
    dut.new_ip = 0x8;
    cycle(2);
    dut.load_new_ip = 1;
    cycle();
    dut.load_new_ip = 0;
    cycle(20);
    ASSERT_EQ(fifo_bytes, (std::vector<uint8_t>{0x0d, 0xf0, 0xce, 0xfa}));
}

TEST_F(PrefetchTestFixture, filled_fifo_doesnt_skip)
{
    for (uint32_t m = 0; m < 32; m += 2)
        memory[m] = m;

    while (fifo_bytes.size() != 6)
        cycle();
    dut.fifo_full = 1;
    cycle(128);
    dut.fifo_full = 0;
    cycle();

    while (fifo_bytes.size() != 8)
        cycle();

    ASSERT_EQ(fifo_bytes, (std::vector<uint8_t>{0, 0, 2, 0, 4, 0, 6, 0}));
}
