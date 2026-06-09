#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xls20::parser {

// ── Data structures ───────────────────────────────────────────────────────────

/// Decoded fields from a 64-char hex XLS-20 NFTokenID.
struct NFTokenIDDecoded {
    uint16_t    flags{0};         ///< Low-flag bits (transferable, burnable, …)
    uint16_t    transfer_fee{0};  ///< In units of 1/50000 (e.g. 1000 = 2%)
    std::string issuer_id;        ///< 40-char hex XRPL AccountID of the issuer
    uint32_t    taxon{0};         ///< Unscrambled collection taxon
    uint32_t    sequence{0};      ///< Issuer-side NFT sequence number
};

/// A confirmed, on-ledger XLS-20 NFT sale extracted from NFTokenAcceptOffer.
struct NFTSale {
    std::string nft_id;          ///< 64-char hex NFTokenID
    std::string collection_key;  ///< "<issuer_id>:<taxon>"
    double      price_xrp{0.0}; ///< Sale price in XRP (drops already divided)
    uint64_t    ledger_time{0}; ///< Unix epoch seconds of ledger close
    std::string buyer;           ///< Account that accepted (took) the offer
    std::string seller;          ///< Account that originally created the offer
};

// ── TxFilter ──────────────────────────────────────────────────────────────────

/**
 * @brief Stateless filter that extracts XLS-20 NFT sales from raw XRPL JSON.
 *
 * Call filter() with each raw WebSocket frame.  Returns an NFTSale only when
 * the frame is a successful NFTokenAcceptOffer with a non-zero XRP price.
 *
 * Thread safety: instances are stateless; all methods are const.
 */
class TxFilter {
public:
    /**
     * @brief Parse a raw XRPL transaction frame.
     * @return NFTSale on a successful XLS-20 sale; std::nullopt otherwise.
     */
    [[nodiscard]]
    std::optional<NFTSale> filter(std::string_view json_str) const;

    /// Decode a 64-char hex NFTokenID into its component fields.
    [[nodiscard]]
    static NFTokenIDDecoded decode_nft_id(const std::string& hex_id);

    /**
     * @brief Recover the original taxon from the scrambled value in NFTokenID.
     *
     * XRPL XOR-scrambles the taxon to prevent sequential enumeration of
     * a collection.  The mask is: (2654435769 × sequence) mod 2³²,
     * where 2654435769 ≈ φ × 2³² (golden-ratio constant).
     */
    [[nodiscard]]
    static uint32_t unscramble_taxon(uint32_t scrambled, uint32_t sequence);
};

} // namespace xls20::parser
