// SHA-256: the FIPS 180-4 test vectors, messages that end on and around
// the 512-bit block boundaries, and the incremental hasher fed in pieces of
// every size giving the same digest as the one-shot functions.

#include "../math/check.hpp"

#include <history/sha256.hpp>

#include <cstddef>
#include <string>

using namespace volt;

namespace
{
    void standard_vectors()
    {
        CHECK(history::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(history::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        // 448 bits: the padding no longer fits in the block.
        CHECK(history::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        // 896 bits.
        CHECK(history::sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                                  "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

        const std::string million(1'000'000, 'a');
        CHECK(history::sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

        // UTF-8 is bytes like any other.
        CHECK(history::sha256_hex("\xC3\x9C") == "913d7314efae6e928bc40f303f3860e56c842124ee6430b1e9d84bec8397e146");
    }

    void block_boundaries()
    {
        struct expected_hash
        {
            std::size_t length;
            const char *hex;
        };
        // n times 'a'. 55 is the longest message whose padding fits in one
        // block, 56 the shortest that needs a second, 64 exactly one block.
        constexpr expected_hash cases[] = {
            {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
            {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
            {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
            {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
            {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
            {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
            {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
            {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
            {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
            {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
        };
        for (const expected_hash &c : cases)
        {
            const std::string message(c.length, 'a');
            CHECK(history::sha256_hex(message) == c.hex);

            // Split at every point.
            for (std::size_t split = 0; split <= message.size(); ++split)
            {
                history::sha256 hasher;
                hasher.update(std::string_view(message).substr(0, split));
                hasher.update(std::string_view(message).substr(split));
                CHECK(history::to_hex(hasher.finish()) == c.hex);
            }
        }
    }

    void incremental()
    {
        std::string message;
        for (int i = 0; i < 1000; ++i)
            message += static_cast<char>(i * 7 + 3);
        const history::sha256_digest whole = history::sha256_of(message);
        CHECK(history::to_hex(whole) == history::sha256_hex(message));

        // In pieces of every size from 1 to 130 bytes.
        for (std::size_t piece = 1; piece <= 130; ++piece)
        {
            history::sha256 hasher;
            for (std::size_t at = 0; at < message.size(); at += piece)
                hasher.update(std::string_view(message).substr(at, piece));
            CHECK(hasher.finish() == whole);
        }

        // Empty pieces change nothing.
        history::sha256 hasher;
        hasher.update("");
        hasher.update("ab");
        hasher.update("");
        hasher.update("c");
        CHECK(history::to_hex(hasher.finish()) == history::sha256_hex("abc"));

        // finish starts over.
        CHECK(history::to_hex(hasher.finish()) == history::sha256_hex(""));
        hasher.update("abc");
        CHECK(history::to_hex(hasher.finish()) == history::sha256_hex("abc"));

        // Every byte value.
        std::string bytes;
        for (int i = 0; i < 256; ++i)
            bytes += static_cast<char>(i);
        CHECK(history::sha256_hex(bytes) == "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");
    }

    void hex()
    {
        history::sha256_digest digest{};
        digest[0] = 0x01;
        digest[1] = 0xAB;
        digest[31] = 0xF0;
        const std::string text = history::to_hex(digest);
        CHECK(text.size() == 64);
        CHECK(text.substr(0, 4) == "01ab");
        CHECK(text.substr(62) == "f0");
    }
} // namespace

int main()
{
    standard_vectors();
    block_boundaries();
    incremental();
    hex();
    return volt::tests::result("history sha256");
}
