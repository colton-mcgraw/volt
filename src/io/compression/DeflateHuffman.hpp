#pragma once

#include "BitStream.hpp"
#include "CanonicalHuffman.hpp"

#include <string>
#include <vector>

namespace volt::io::compression::detail {

using DeflateBitReader = BitReader<BitOrder::kLsbFirst>;
using DeflateHuffmanTable = CanonicalHuffmanTable;

bool decodeDeflateHuffmanSymbol(DeflateBitReader& reader,
                                const DeflateHuffmanTable& table,
                                int& symbol);

bool buildFixedDeflateHuffmanTables(DeflateHuffmanTable& litLenTable,
                                    DeflateHuffmanTable& distanceTable,
                                    std::string& error);

bool buildDynamicDeflateHuffmanTables(DeflateBitReader& reader,
                                      DeflateHuffmanTable& litLenTable,
                                      DeflateHuffmanTable& distanceTable,
                                      std::string& error);

}  // namespace volt::io::compression::detail
