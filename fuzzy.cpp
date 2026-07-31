#include "fuzzy.h"
#include <algorithm>
#include <limits>

int FuzzyMatcher::levenshtein(const std::string& a, const std::string& b) {
    const size_t m = a.size();
    const size_t n = b.size();

    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));

    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({
                dp[i - 1][j] + 1,
                dp[i][j - 1] + 1,
                dp[i - 1][j - 1] + cost
            });
        }
    }
    return dp[m][n];
}

std::string FuzzyMatcher::find_closest(const std::string& input,
                                        const std::map<std::string, SdkEntry>& commands,
                                        int max_distance) {
    std::string best;
    int best_dist = std::numeric_limits<int>::max();

    for (const auto& [key, _] : commands) {
        int dist = levenshtein(input, key);
        if (dist < best_dist) {
            best_dist = dist;
            best = key;
        }
    }

    if (best_dist <= max_distance) return best;
    return "";
}

std::vector<std::string> FuzzyMatcher::find_by_category(
    const std::string& category,
    const std::map<std::string, SdkEntry>& commands)
{
    std::vector<std::string> result;
    for (const auto& [key, entry] : commands) {
        if (entry.category == category) {
            result.push_back(key);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
