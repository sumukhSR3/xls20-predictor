#include "parser/tx_filter.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace xls20::parser {

using json = nlohmann::json;

// ── Constants ─────────────────────────────────────────────────────────────────

/// Seconds between the Ripple epoch (Jan 1 2000) and the Unix epoch (Jan 1 1970).
static constexpr uint64_t kRippleEpochOffset = 946'684'800ULL;

/// One XRP = 1 000 000 drops.
static constexpr double kDropsPerXRP = 1'000'000.0;

// ── Internal hex helpers ──────────────────────────────────────────────────────

static uint16_t hex_chars_to_u16(const std::string& s, size_t offset) {
    uint16_t val = 0;
    for (size_t i = 0; i < 4; ++i) {
        char c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(s[offset + i])));
        uint16_t nibble = (c >= 'A') ? uint16_t(c - 'A' + 10)
                                     : uint16_t(c - '0');
        val = static_cast<uint16_t>((val << 4) | nibble);
    }
    return val;
}

static uint32_t hex_chars_to_u32(const std::string& s, size_t offset) {
    uint32_t val = 0;
    for (size_t i = 0; i < 8; ++i) {
        char c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(s[offset + i])));
        uint32_t nibble = (c >= 'A') ? uint32_t(c - 'A' + 10)
                                     : uint32_t(c - '0');
        val = (val << 4) | nibble;
    }
    return val;
}

// ── NFTokenID decode ──────────────────────────────────────────────────────────

/*
 * NFTokenID wire layout (64 hex chars = 32 bytes):
 *
 *   Chars  0– 3  :  Flags (2 bytes)
 *   Chars  4– 7  :  TransferFee (2 bytes, units of 1/50 000)
 *   Chars  8–47  :  Issuer AccountID (20 bytes)
 *   Chars 48–55  :  Scrambled Taxon (4 bytes)
 *   Chars 56–63  :  Token Sequence (4 bytes)
 */
NFTokenIDDecoded TxFilter::decode_nft_id(const std::string& hex_id) {
    if (hex_id.size() != 64)
        throw std::invalid_argument(
            "NFTokenID must be exactly 64 hex chars; got length " +
            std::to_string(hex_id.size()));

    NFTokenIDDecoded d;
    d.flags        = hex_chars_to_u16(hex_id, 0);
    d.transfer_fee = hex_chars_to_u16(hex_id, 4);
    d.issuer_id    = hex_id.substr(8, 40);   // 20 bytes → 40 hex chars
    uint32_t scrambled = hex_chars_to_u32(hex_id, 48);
    d.sequence         = hex_chars_to_u32(hex_id, 56);
    d.taxon            = unscramble_taxon(scrambled, d.sequence);
    return d;
}

uint32_t TxFilter::unscramble_taxon(uint32_t scrambled, uint32_t sequence) {
    // Mask = (golden-ratio constant × sequence) mod 2³²
    // Constant ≈ φ × 2³² = 2654435769
    uint32_t mask = static_cast<uint32_t>(2'654'435'769ULL * sequence);
    return scrambled ^ mask;
}

// ── TxFilter::filter ──────────────────────────────────────────────────────────

std::optional<NFTSale> TxFilter::filter(std::string_view json_str) const {
    // ── 1. Parse JSON ──────────────────────────────────────────────────────────
    json msg;
    try {
        msg = json::parse(json_str);
    } catch (const json::exception&) {
        return std::nullopt;  // malformed frame — silently skip
    }

    // ── 2. Only handle "transaction" type stream messages ─────────────────────
    auto type_it = msg.find("type");
    if (type_it == msg.end() || *type_it != "transaction")
        return std::nullopt;

    const json& tx   = msg.value("transaction", json{});
    const json& meta = msg.value("meta",        json{});

    if (tx.empty() || meta.empty()) return std::nullopt;

    // ── 3. Must be NFTokenAcceptOffer ─────────────────────────────────────────
    if (tx.value("TransactionType", std::string{}) != "NFTokenAcceptOffer")
        return std::nullopt;

    // ── 4. Must have succeeded on-ledger ─────────────────────────────────────
    if (meta.value("TransactionResult", std::string{}) != "tesSUCCESS")
        return std::nullopt;

    // ── 5. Extract NFTokenID + Amount from the deleted NFTokenOffer ───────────
    const json& affected = meta.value("AffectedNodes", json::array());

    std::string nft_id;
    double      price_xrp = 0.0;
    std::string seller;

    for (const auto& node : affected) {
        auto del_it = node.find("DeletedNode");
        if (del_it == node.end()) continue;
        const json& dn = *del_it;

        if (dn.value("LedgerEntryType", std::string{}) != "NFTokenOffer")
            continue;

        const json& fields = dn.value("FinalFields", json{});
        nft_id = fields.value("NFTokenID", std::string{});
        seller = fields.value("Owner",     std::string{});

        // Amount is either a drops string (XRP) or an IOU value object
        const json& amount = fields.value("Amount", json{});
        if (amount.is_string()) {
            try {
                double drops = std::stod(amount.get<std::string>());
                price_xrp = drops / kDropsPerXRP;
            } catch (...) {}
        } else if (amount.is_object()) {
            try {
                price_xrp = std::stod(amount.value("value", std::string{"0"}));
            } catch (...) {}
        }
        break;  // only need the first matching offer
    }

    if (nft_id.size() != 64 || price_xrp <= 0.0)
        return std::nullopt;

    // ── 6. Decode NFTokenID → collection key ──────────────────────────────────
    NFTokenIDDecoded decoded;
    try {
        decoded = decode_nft_id(nft_id);
    } catch (const std::exception& e) {
        std::cerr << "[TxFilter] NFTokenID decode failed (" << nft_id
                  << "): " << e.what() << '\n';
        return std::nullopt;
    }

    // ── 7. Resolve ledger close time → Unix epoch ────────────────────────────
    uint64_t ledger_close = 0;
    if (tx.contains("date") && tx["date"].is_number_unsigned()) {
        // "date" is Ripple epoch seconds
        ledger_close = tx["date"].get<uint64_t>() + kRippleEpochOffset;
    }
    if (ledger_close == 0) {
        ledger_close = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
            .count());
    }

    // ── 8. Build and return the NFTSale ───────────────────────────────────────
    NFTSale sale;
    sale.nft_id         = nft_id;
    sale.collection_key = decoded.issuer_id + ":" + std::to_string(decoded.taxon);
    sale.price_xrp      = price_xrp;
    sale.ledger_time    = ledger_close;
    sale.buyer          = tx.value("Account", std::string{});
    sale.seller         = seller;

    return sale;
}

} // namespace xls20::parser
