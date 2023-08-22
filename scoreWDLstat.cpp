#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "external/chess.hpp"
#include "external/json.hpp"
#include "external/threadpool.hpp"

namespace fs = std::filesystem;

using namespace chess;

enum class Result { WIN = 'W', DRAW = 'D', LOSS = 'L' };

struct ResultKey {
    Result white;
    Result black;
};

struct Key {
    Result outcome;             // game outcome from PoV of side to move
    int move, material, score;  // move number, material count, engine's eval
    bool operator==(const Key &k) const {
        return outcome == k.outcome && move == k.move && material == k.material && score == k.score;
    }
    operator std::size_t() const {
        // golden ratio hashing, thus 0x9e3779b9
        std::uint32_t hash = static_cast<int>(outcome);
        hash ^= move + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= material + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= score + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
    operator std::string() const {
        return "('" + std::string(1, static_cast<char>(outcome)) + "', " + std::to_string(move) +
               ", " + std::to_string(material) + ", " + std::to_string(score) + ")";
    }
};

// overload the std::hash function for Key
template <>
struct std::hash<Key> {
    std::size_t operator()(const Key &k) const { return static_cast<std::size_t>(k); }
};

// unordered map to count (outcome, move, material, score) tuples in pgns
using map_t = std::unordered_map<Key, int>;

std::atomic<std::size_t> total_chunks = 0;

namespace analysis {

/// @brief Custom stof implementation to avoid locale issues, once clang supports std::from_chars
/// for floats this can be removed
/// @param str
/// @return
float fast_stof(const char *str) {
    float result   = 0.0f;
    int sign       = 1;
    int decimal    = 0;
    float fraction = 1.0f;

    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    // Convert integer part
    while (*str >= '0' && *str <= '9') {
        result = result * 10.0f + (*str - '0');
        str++;
    }

    // Convert decimal part
    if (*str == '.') {
        str++;
        while (*str >= '0' && *str <= '9') {
            result = result * 10.0f + (*str - '0');
            fraction *= 10.0f;
            str++;
        }
        decimal = 1;
    }

    // Apply sign and adjust for decimal
    result *= sign;
    if (decimal) {
        result /= fraction;
    }

    return result;
}

/// @brief Magic value for fishtest pgns, ~1.2 million keys
static constexpr int map_size = 1200000;

/// @brief Analyze a single game and update the position map
/// @param pos_map
/// @param game
void ana_game(map_t &pos_map, const std::optional<Game> &game) {
    if (game.value().headers().find("Result") == game.value().headers().end()) {
        return;
    }

    const auto result = game.value().headers().at("Result");

    ResultKey resultkey;

    if (result == "1-0") {
        resultkey.white = Result::WIN;
        resultkey.black = Result::LOSS;
    } else if (result == "0-1") {
        resultkey.white = Result::LOSS;
        resultkey.black = Result::WIN;
    } else if (result == "1/2-1/2") {
        resultkey.white = Result::DRAW;
        resultkey.black = Result::DRAW;
    } else {
        return;
    }

    Board board = Board();

    if (game.value().headers().find("FEN") != game.value().headers().end()) {
        board.setFen(game.value().headers().at("FEN"));
    }

    if (game.value().headers().find("Variant") != game.value().headers().end() &&
        game.value().headers().at("Variant") == "fischerandom") {
        board.set960(true);
    }

    int ply = 0;

    for (const auto &move : game.value().moves()) {
        if (++ply > 400) {
            break;
        }

        const size_t delimiter_pos = move.comment.find('/');

        Key key;
        key.score = 1002;

        if (delimiter_pos != std::string::npos && move.comment != "book") {
            const auto match_score = move.comment.substr(0, delimiter_pos);

            if (match_score[1] == 'M') {
                if (match_score[0] == '+') {
                    key.score = 1001;
                } else {
                    key.score = -1001;
                }

            } else {
                int score = 100 * fast_stof(match_score.c_str());

                if (score > 1000) {
                    score = 1000;
                } else if (score < -1000) {
                    score = -1000;
                }

                key.score = int(std::floor(score / 5.0)) * 5;  // reduce precision
            }
        }

        if (key.score != 1002) {  // a score was found
            key.outcome = board.sideToMove() == Color::WHITE ? resultkey.white : resultkey.black;
            key.move    = (ply + 1) / 2;  // move number
            const auto knights = builtin::popcount(board.pieces(PieceType::KNIGHT));
            const auto bishops = builtin::popcount(board.pieces(PieceType::BISHOP));
            const auto rooks   = builtin::popcount(board.pieces(PieceType::ROOK));
            const auto queens  = builtin::popcount(board.pieces(PieceType::QUEEN));
            const auto pawns   = builtin::popcount(board.pieces(PieceType::PAWN));
            key.material       = 9 * queens + 5 * rooks + 3 * bishops + 3 * knights + pawns;
            pos_map[key] += 1;
        }

        board.makeMove(move.move);
    }
}

void ana_files(map_t &map, const std::vector<std::string> &files) {
    map.reserve(map_size);

    for (const auto &file : files) {
        std::ifstream pgn_file(file);
        std::string line;

        while (true) {
            auto game = pgn::readGame(pgn_file);

            if (!game.has_value()) {
                break;
            }

            ana_game(map, game);
        }

        pgn_file.close();
    }
}

}  // namespace analysis

/// @brief Get all files from a directory.
/// @param path
/// @return
[[nodiscard]] std::vector<std::string> get_files(std::string_view path = "./pgns") {
    std::vector<std::string> files;

    for (const auto &entry : fs::directory_iterator(path)) {
        if (entry.path().extension() == ".pgn") {
            files.push_back(entry.path().string());
        }
    }

    return files;
}

/// @brief Split into successive n-sized chunks from pgns.
/// @param pgns
/// @param target_chunks
/// @return
[[nodiscard]] std::vector<std::vector<std::string>> split_chunks(
    const std::vector<std::string> &pgns, int target_chunks) {
    const int chunks_size = (pgns.size() + target_chunks - 1) / target_chunks;

    auto begin = pgns.begin();
    auto end   = pgns.end();

    std::vector<std::vector<std::string>> chunks;

    while (begin != end) {
        auto next =
            std::next(begin, std::min(chunks_size, static_cast<int>(std::distance(begin, end))));
        chunks.push_back(std::vector<std::string>(begin, next));
        begin = next;
    }

    return chunks;
}

/// @brief
/// @param argc
/// @param argv Possible ones are --dir and --file
/// @return
int main(int argc, char const *argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    std::vector<std::string> files_pgn;

    if (std::find(args.begin(), args.end(), "--dir") != args.end()) {
        const auto path = std::find(args.begin(), args.end(), "--dir") + 1;
        files_pgn       = get_files(*path);
    } else if (std::find(args.begin(), args.end(), "--file") != args.end()) {
        const auto path = std::find(args.begin(), args.end(), "--file") + 1;
        files_pgn       = {*path};
    } else {
        files_pgn = get_files();
    }

    // Create more chunks than threads to prevent threads from idling.
    int target_chunks = 4 * std::max(1, int(std::thread::hardware_concurrency()));

    auto files_chunked = split_chunks(files_pgn, target_chunks);

    std::cout << "Found " << files_pgn.size() << " pgn files, creating " << files_chunked.size()
              << " chunks for processing." << std::endl;

    map_t pos_map;
    pos_map.reserve(analysis::map_size);

    // Mutex for pos_map access
    std::mutex map_mutex;

    // Create a thread pool
    ThreadPool pool(std::thread::hardware_concurrency());

    // Print progress
    std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size() << std::flush;

    const auto t0 = std::chrono::high_resolution_clock::now();

    for (const auto &files : files_chunked) {
        pool.enqueue([&files, &map_mutex, &pos_map, &files_chunked]() {
            map_t map;
            analysis::ana_files(map, files);

            total_chunks++;

            // Limit the scope of the lock
            {
                const std::lock_guard<std::mutex> lock(map_mutex);

                for (const auto &pair : map) {
                    pos_map[pair.first] += pair.second;
                }

                // Print progress
                std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size()
                          << std::flush;
            }
        });
    }

    // Wait for all threads to finish
    pool.wait();

    const auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "\nTime taken: "
              << std::chrono::duration_cast<std::chrono::seconds>(t1 - t0).count() << "s"
              << std::endl;

    std::uint64_t total = 0;

    nlohmann::json j;

    for (const auto &pair : pos_map) {
        const auto map_key_t = static_cast<std::string>(pair.first);
        j[map_key_t]         = pair.second;
        total += pair.second;
    }

    // save json to file
    std::ofstream outFile("scoreWDLstat.json");
    outFile << j.dump(2);
    outFile.close();

    std::cout << "Retained " << total << " scored positions for analysis." << std::endl;

    return 0;
}