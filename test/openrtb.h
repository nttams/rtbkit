#pragma once

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <optional>

struct Banner
{
    ~Banner();

    std::vector<int> w;
    std::vector<int> h;
    int wmax;
    int hmax;
    int wmin;
    int hmin;
    std::string id;
};

struct Video
{
    ~Video();
    double minduration;
    double maxduration;
    int w;
    int h;
    int startdelay;
    int sequence;
    int maxextended;
    int minbitrate;
    int maxbitrate;
    bool boxingallowed;
    std::vector<Banner> companionad;
};

struct Publisher
{
    ~Publisher();
    std::string id;
    std::string name;
    std::string domain;
};

typedef Publisher Producer;

struct Deal
{
    ~Deal();
    std::string id;
    double bidfloor;
    std::string bidfloorcur;
    std::vector<std::string> wseat;
    std::vector<std::string> wadomain;
    int at;
};

struct PMP
{
    ~PMP();
    int privateAuction;
    std::vector<Deal> deals;
};

struct Impression
{
    ~Impression();
    std::string id;
    std::optional<Banner> banner;
    std::optional<Video> video;
    std::string displaymanager;
    std::string displaymanagerver;
    bool instl;
    std::string tagid;
    double bidfloor;
    std::string bidfloorcur;
    int secure;
    std::vector<std::string> iframebuster;
    std::optional<PMP> pmp;
};

struct Content
{
    ~Content();
    std::string id;
    int episode;
    std::string title;
    std::string series;
    std::string season;
    std::string url;
    std::vector<std::string> keywords;
    std::string contentrating;
    std::string userrating;
    std::string context;
    bool livestream;
    std::optional<Producer> producer;
    int len;
    std::string language;
};

struct Context
{
    ~Context();
    std::string id;
    std::string name;
    std::string domain;
    bool privacypolicy;
    std::optional<Publisher> publisher;
    std::optional<Content> content;
    std::vector<std::string> keywords;
};

struct SiteInfo
{
    std::string page;
    std::string ref;
    std::string search;
};

struct Site : public Context, public SiteInfo
{
};

struct AppInfo
{
    std::string ver;
    std::string bundle;
    bool paid;
    std::string storeurl;
};

struct App : public Context, public AppInfo
{
};

struct Geo
{
    ~Geo();
    double lat;
    double lon;
    std::string country;
    std::string region;
    std::string regionfips104;
    std::string metro;
    std::string city;
    std::string zip;
};

struct Device
{
    ~Device();
    bool dnt;
    std::string ua;
    std::string ip;
    std::optional<Geo> geo;
    std::string didsha1;
    std::string didmd5;
    std::string dpidsha1;
    std::string dpidmd5;
    std::string macsha1;
    std::string macmd5;
    std::string ipv6;
    std::string carrier;
    std::string language;
    std::string make;
    std::string model;
    std::string os;
    std::string osv;
    bool js;
    std::string flashver;
    std::string ifa;
};

struct Segment
{
    std::string id;
    std::string name;
    std::string value;

    double segmentusecost;
};

struct Data
{
    std::string id;
    std::string name;
    std::vector<Segment> segment;

    std::string usecostcurrency;
    double datausecost;
};

struct User
{
    ~User();
    std::string id;
    std::string buyeruid;
    int yob;
    std::string gender;
    std::vector<std::string> keywords;
    std::string customdata;
    std::optional<Geo> geo;
    std::vector<Data> data;

    int tz;
    int sessiondepth;
};

struct Regulations
{
    ~Regulations();
    int coppa;
};

struct BidRequest
{
    ~BidRequest();
    std::string id;
    std::vector<Impression> imp;

    std::optional<Site> site;
    std::optional<App> app;
    std::optional<Device> device;
    std::optional<User> user;
    int at;
    int tmax;
    std::vector<std::string> wseat;
    bool allimps;
    std::vector<std::string> cur;
    std::vector<std::string> badv;
    std::optional<Regulations> regs;
};

//
// Response
//

struct Bid
{
    std::string id;
    std::string impid;
    double price;
    std::string adid;
    std::string nurl;
    std::string adm;
    std::vector<std::string> adomain;
    std::string iurl;
    std::string cid;
    std::string crid;
    std::string dealid;

    int w;
    int h;
};

struct SeatBid
{
    std::vector<Bid> bid;
    std::string seat;
    int group;
};

struct BidResponse
{
    std::string id;
    std::vector<SeatBid> seatbid;
    std::string bidid;
    std::string cur;
    std::string customData;
};