#include "xserver/weather_parse.h"
#include <gtest/gtest.h>
#include <string>

using namespace fbx::xserver;

namespace {
// wttr.in's layout (?format=j1): keys sorted, "value" labels everything.
std::string hours(const std::string& midday) {
    std::string h = "\"hourly\":[";
    for (int i = 0; i < 8; ++i)
        h += std::string(i ? "," : "") + "{\"time\":\"" + std::to_string(i * 300) + "\",\"weatherDesc\":[{\"value\":\"" +
             (i == 4 ? midday : "Night") + "\"}],\"weatherIconUrl\":[{\"value\":\"\"}]}";
    return h + "]";
}
std::string day(const std::string& date, const std::string& max, const std::string& midday) {
    return "{\"astronomy\":[{\"moonrise\":\"x\"}],\"avgtempC\":\"1\",\"date\":\"" + date + "\"," + hours(midday) +
           ",\"maxtempC\":\"" + max + "\",\"maxtempF\":\"70\",\"mintempC\":\"2\",\"mintempF\":\"35\"}";
}
const std::string kReport =
    "{\"current_condition\":[{\"temp_C\":\"11\",\"weatherDesc\":[{\"value\":\"Light rain \"}],"
    "\"weatherIconUrl\":[{\"value\":\"\"}]}],"
    "\"nearest_area\":[{\"areaName\":[{\"value\":\"Chongdong\"}],\"country\":[{\"value\":\"South Korea\"}],"
    "\"region\":[{\"value\":\"Seoul\"}]}],"
    "\"request\":[{\"query\":\"Lat 37 and Lon 127\",\"type\":\"LatLon\"}],"
    "\"weather\":[" + day("2026-11-05", "16", "Partly Cloudy ") + "," + day("2026-11-06", "15", "Sunny") + "," +
    day("2026-11-07", "12", "Overcast") + "]}";
}  // namespace

TEST(WeatherParse, AreaIsItsValueNotTheWordValue) {
    EXPECT_EQ(wttr::parse(kReport).area, "Chongdong");
}

TEST(WeatherParse, CurrentConditionTrimmed) {
    EXPECT_EQ(wttr::parse(kReport).desc, "Light rain");
}

TEST(WeatherParse, ForecastDescribesTheDayNotThePlace) {
    auto r = wttr::parse(kReport);
    ASSERT_EQ(r.days.size(), 3u);
    EXPECT_EQ(r.days[0].date, "2026-11-05");
    EXPECT_EQ(r.days[0].desc, "Partly Cloudy");
    EXPECT_EQ(r.days[1].desc, "Sunny");
    EXPECT_EQ(r.days[2].desc, "Overcast");
    EXPECT_EQ(r.days[0].max_c, "16");
    EXPECT_EQ(r.days[1].max_c, "15");
    EXPECT_EQ(r.days[2].min_f, "35");
}

TEST(WeatherParse, MissingPartsLeaveThemEmpty) {
    auto r = wttr::parse("{\"current_condition\":[{\"temp_C\":\"1\"}]}");
    EXPECT_EQ(r.area, "");
    EXPECT_EQ(r.desc, "");
    EXPECT_TRUE(r.days.empty());
    EXPECT_TRUE(wttr::parse("").days.empty());
}

TEST(WeatherParse, ADayWithFewerHoursDoesntBorrowTheNextDays) {
    const std::string j = "{\"weather\":[{\"date\":\"a\",\"hourly\":[{\"weatherDesc\":[{\"value\":\"Fog\"}]}],"
                          "\"maxtempC\":\"1\"},{\"date\":\"b\",\"hourly\":[{\"weatherDesc\":[{\"value\":\"Snow\"}]}]}]}";
    auto r = wttr::parse(j);
    ASSERT_EQ(r.days.size(), 2u);
    EXPECT_EQ(r.days[0].desc, "Fog");
    EXPECT_EQ(r.days[1].desc, "Snow");
}
