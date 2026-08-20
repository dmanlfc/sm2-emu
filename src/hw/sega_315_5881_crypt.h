// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315-5881_crypt.cpp/.h
// (BSD-3-Clause, copyright-holders Andreas Naive, Olivier Galibert,
// David Haywood).
#pragma once

#include "core/types.h"

#include <functional>
#include <utility>
#include <vector>

namespace sm2::hw {

/// Sega 315-5881 stream cipher, as used on Model 2 security boards.
///
/// The chip decrypts a stream the program has already staged in the 64 KB RAM
/// window next to it, so it needs a read callback rather than direct ROM
/// access. Output is a counter-mode stream over a 16-bit block cipher built
/// from two 4-round Feistel networks; a stream is split into substreams, each
/// introduced by an 18-bit header, and a substream may additionally be
/// LZ-style compressed with a fixed set of Huffman trees.
///
/// The header's two leading bits shift the plaintext two bits out of step with
/// the block cipher's own word boundaries, which is why `get_decrypted_16`
/// keeps the previous word around and only takes two fresh bits per call.
class Sega3155881Crypt {
public:
    /// One 16-bit word of ciphertext at word address `addr`, big-endian.
    using ReadCallback = std::function<u16(u32)>;

    Sega3155881Crypt();

    void set_read_callback(ReadCallback read) { m_read = std::move(read); }
    void set_key(u32 key) { m_key = key; }

    void reset();

    // -- register interface (MAME's iomap_le, at 0x01d90000 on Model 2) -----

    /// Bit 0 is a busy flag. The real chip is always ready by the time the
    /// program looks, so MAME returns zero unconditionally.
    [[nodiscard]] u16 ready_r() const { return 0; }

    void addrlo_w(u16 data);
    void addrhi_w(u16 data);
    void subkey_le_w(u16 data);
    void subkey_be_w(u16 data);

    [[nodiscard]] u16 decrypt_le_r();
    [[nodiscard]] u16 decrypt_be_r();

    void set_addr_low(u16 data);
    void set_addr_high(u16 data);
    void set_subkey(u16 data);

    // -- block cipher ------------------------------------------------------

    struct Sbox {
        u8  table[64];
        int inputs[6];   ///< Bit positions of the inputs, -1 for key-only.
        int outputs[2];  ///< Bit positions of the outputs.
    };

    /// The block cipher, stateless apart from its keys. Public because it is
    /// the one part of the chip a test can exercise on its own.
    [[nodiscard]] static u16 block_decrypt(u32 game_key, u16 sequence_key, u16 counter,
                                           u16 data);

    [[nodiscard]] static const Sbox& fn1_sbox(int round, int index);
    [[nodiscard]] static const Sbox& fn2_sbox(int round, int index);

private:
    /// MAME notes that this ought to be a stream rather than a buffer; two
    /// bytes is the smallest size that keeps its block bookkeeping honest.
    static constexpr int kBufferSize = 2;
    static constexpr int kLineSize   = 512;

    static constexpr u32 kFlagCompressed = 0x20000;

    [[nodiscard]] static int feistel_function(int input, const Sbox* sboxes, u32 subkeys);

    [[nodiscard]] u16 do_decrypt();
    [[nodiscard]] u16 get_decrypted_16();
    [[nodiscard]] int get_compressed_bit();

    void enc_start();
    void enc_fill();
    void line_fill();

    ReadCallback m_read;
    u32          m_key = 0;

    bool m_first_read = false;

    std::vector<u8> m_buffer;
    std::vector<u8> m_line_buffer;
    std::vector<u8> m_line_buffer_prev;

    u32 m_prot_cur_address = 0;
    u16 m_subkey           = 0;
    u16 m_dec_hist         = 0;
    u32 m_dec_header       = 0;

    bool m_enc_ready = false;

    int m_buffer_pos      = 0;
    int m_line_buffer_pos = 0;
    int m_line_buffer_size = 0;
    int m_buffer_bit       = 0;
    int m_buffer_bit2      = 0;
    u8  m_buffer2[2]{};
    u16 m_buffer2a = 0;

    int m_block_size       = 0;
    int m_block_pos        = 0;
    int m_block_numlines   = 0;
    int m_done_compression = 0;
};

}  // namespace sm2::hw
