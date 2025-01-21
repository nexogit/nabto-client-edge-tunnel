#include <nabto_client.hpp>
#include <nabto/nabto_client_experimental.h>
#include "httplib.h"
#include "pairing.hpp"
#include "config.hpp"
#include "timestamp.hpp"
#include "iam.hpp"
#include "iam_interactive.hpp"
#include "version.hpp"
#include <sstream> // Per std::ostringstream
#include <3rdparty/cxxopts.hpp>
#include <3rdparty/nlohmann/json.hpp>

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <thread>
#include <future>
#include <map>

using json = nlohmann::json;

const std::string appName = "edge_tunnel_client";

enum {
  COAP_CONTENT_FORMAT_APPLICATION_CBOR = 60
};

// TODO reconnect when connection is closed.

std::string generalHelp = R"(This client application is designed to be used with a tcp tunnel
device application. The functionality of the system is to enable
tunnelling of TCP connections over the internet. The system allows a
TCP client on the client side to connect to a TCP service on the
device side. On the client side a TCP listener is created which
listens for connections to localhost:<local-port>, when a TCP
connection is made from an application on the client side to
localhost:<local-port> the TCP connection is tunnelled to the service
on the device.

Example usage based on ssh:

 0. Run a tcp tunnel device on a system with an ssh server.
 1. Pair the client with the device. edge_tunnel_client --pair
 2. Create a tunnel to the SSH service on the device. edge_tunnel_client --service ssh --local-port <port>
 3. Connect to the SSH server through the tunnel. On the client side: ssh 127.0.0.1 -p <port>.
    A SSH connection is now opened to the ssh server running on the device.
)";

void PrintGeneralHelp()
{
    std::cout << generalHelp << std::endl;
}

class MyLogger : public nabto::client::Logger
{
 public:
    void log(nabto::client::LogMessage message) {
        std::cout << time_in_HH_MM_SS_MMM() << " [" << message.getSeverity() << "] - " << message.getMessage() << std::endl;
    }
};

std::shared_ptr<nabto::client::Connection> connection_;

void signalHandler(int s){
    printf("Caught signal %d\n",s);
    if (connection_) {
        connection_->close()->waitForResult();
    }
}

static void printMissingClientConfig(const std::string& filename)
{
    std::cerr
        << "The example is missing the client configuration file (" << filename << ")." << std::endl
        << "The client configuration file is a json file which can be" << std::endl
        << "used to change the server URL used for remote connections." << std::endl
        << "In normal scenarios, the file should simply contain an" << std::endl
        << "empty json document:"
        << "{" << std::endl
        << "}" <<std::endl;

}

class CloseListener : public nabto::client::ConnectionEventsCallback {
 public:

    CloseListener() {
    }
    void onEvent(int event) {
        if (event == NABTO_CLIENT_CONNECTION_EVENT_CLOSED) {
            std::cout << "Connection closed, closing application" << std::endl;
            promise_.set_value();
            return;
        }
    }

    void waitForClose() {
        auto future = promise_.get_future();
        future.get();
    }

 private:
    std::promise<void> promise_;
};

void handleFingerprintMismatch(std::shared_ptr<nabto::client::Connection> connection, Configuration::DeviceInfo device)
{
    IAM::IAMError ec;
    std::unique_ptr<IAM::PairingInfo> pairingInfo;
    std::tie(ec, pairingInfo) = IAM::get_pairing_info(connection);
    if (ec.ok()) {
        if (pairingInfo->getProductId() != device.getProductId()) {
            std::cerr << "The Product ID of the connected device (" <<  pairingInfo->getProductId() << ") does not match the Product ID for the bookmark " << device.getFriendlyName() << std::endl;
        } else if (pairingInfo->getDeviceId() != device.getDeviceId()) {
            std::cerr << "The Device ID of the connected device (" <<  pairingInfo->getDeviceId() << ") does not match the Device ID for the bookmark " << device.getFriendlyName() << std::endl;
        } else {
            std::cerr << "The public key of the device does not match the public key in the pairing. Repair the device with the client." << std::endl;
        }
    } else {
        // should not happen
        ec.printError();
    }
}

std::shared_ptr<nabto::client::Connection> createConnection(std::shared_ptr<nabto::client::Context> context, Configuration::DeviceInfo device)
{
    auto Config = Configuration::GetConfigInfo();
    if (!Config) {
        printMissingClientConfig(Configuration::GetConfigFilePath());
        return nullptr;
    }

    auto connection = context->createConnection();
    connection->setProductId(device.getProductId());
    connection->setDeviceId(device.getDeviceId());
    connection->setApplicationName(appName);
    connection->setApplicationVersion(edge_tunnel_client_version());

    if (!device.getDirectCandidate().empty()) {
        connection->enableDirectCandidates();
        connection->addDirectCandidate(device.getDirectCandidate(), 5592);
        connection->endOfDirectCandidates();
    }

    std::string privateKey;
    if(!Configuration::GetPrivateKey(context, privateKey)) {
        return nullptr;
    }
    connection->setPrivateKey(privateKey);


    if (!Config->getServerUrl().empty()) {
        connection->setServerUrl(Config->getServerUrl());
    }

    connection->setServerConnectToken(device.getSct());

    try {
        connection->connect()->waitForResult();
    } catch (nabto::client::NabtoException& e) {
        if (e.status().getErrorCode() == nabto::client::Status::NO_CHANNELS) {
            auto localStatus = nabto::client::Status(connection->getLocalChannelErrorCode());
            auto remoteStatus = nabto::client::Status(connection->getRemoteChannelErrorCode());
            std::cerr << "Not Connected." << std::endl;
            std::cerr << " The Local status is: " << localStatus.getDescription() << std::endl;
            std::cerr << " The Remote status is: " << remoteStatus.getDescription() << std::endl;
        } else {
            std::cerr << "Connect failed " << e.what() << std::endl;
        }
        return nullptr;
    }

    try {
        if (connection->getDeviceFingerprint() != device.getDeviceFingerprint()) {
            handleFingerprintMismatch(connection, device);
            return nullptr;
        }
    } catch (...) {
        std::cerr << "Missing device fingerprint in state, pair with the device again" << std::endl;
        return nullptr;
    }

    // we are paired if the connection has a user in the device
    IAM::IAMError ec;
    std::unique_ptr<IAM::User> user;
    std::tie(ec, user) = IAM::get_me(connection);

    if (!user) {
        std::cerr << "The client is not paired with device, do the pairing again" << std::endl;
        return nullptr;
    }
    return connection;
}

static nlohmann::json get_service(std::shared_ptr<nabto::client::Connection> connection, const std::string& service);
static void print_service(const nlohmann::json& service);

std::map<std::string, nlohmann::json> list_services(std::shared_ptr<nabto::client::Connection> connection)
{   
    std::map<std::string, nlohmann::json> services;
    auto coap = connection->createCoap("GET", "/tcp-tunnels/services");
    coap->execute()->waitForResult();
    if (coap->getResponseStatusCode() == 205 &&
        coap->getResponseContentFormat() == COAP_CONTENT_FORMAT_APPLICATION_CBOR)
    {
        auto cbor = coap->getResponsePayload();
        auto data = json::from_cbor(cbor);
        if (data.is_array()) {
            std::cout << "Available services ..." << std::endl;
            try {
                for (auto s : data) {
                    nlohmann::json test = get_service(connection, s);
                    if (!test.is_null()) {
                        services.insert({s.get<std::string>(), test});
                    }
                }
            } catch(std::exception& e) {
                std::cerr << "Failed to get services: " << e.what() << std::endl;
                return {};
            }
        }
    }
    return services;;
}

nlohmann::json get_service(std::shared_ptr<nabto::client::Connection> connection, const std::string& service)
{
    auto coap = connection->createCoap("GET", "/tcp-tunnels/services/" + service);
    coap->execute()->waitForResult();
    if (coap->getResponseStatusCode() == 205 &&
        coap->getResponseContentFormat() == COAP_CONTENT_FORMAT_APPLICATION_CBOR)
    {
        auto cbor = coap->getResponsePayload();
        auto data = json::from_cbor(cbor);
        print_service(data);
        return data;
    }
    return nullptr;
}

std::string constant_width_string(std::string in) {
    const size_t maxLength = 10;
    if (in.size() > maxLength) {
        return in;
    }
    in.append(maxLength - in.size(), ' ');
    return in;
}

void print_service(const nlohmann::json& service)
{
    std::string id = service["Id"].get<std::string>();
    std::string type = service["Type"].get<std::string>();
    std::string host = service["Host"].get<std::string>();
    uint16_t port = service["Port"].get<uint16_t>();
    std::cout << "Service: " << constant_width_string(id) << " Type: " << constant_width_string(type) << " Host: " << host << "  Port: " << port << std::endl;
}

bool split_in_service_and_port(const std::string& in, std::string& service, uint16_t& port)
{
    std::size_t colon = in.find_first_of(":");
    if (colon != std::string::npos) {
        service = in.substr(0,colon);
        std::string portStr = in.substr(colon+1);
        try {
            port = std::stoi(portStr);
        } catch (std::invalid_argument& e) {
            std::cerr << "the format for the service is not correct the string " << in << " cannot be parsed as service:port" << std::endl;
            return false;
        }
    } else {
        port = 0;
        service = in;
    }

    return true;
}




void printDeviceInfo(std::shared_ptr<IAM::PairingInfo> pi)
{
    auto ms = pi->getModes();
    std::cout << "Successfully retrieved device info:" << std::endl
              << "# Product ID   : " << pi->getProductId() << std::endl
              << "# Device ID    : " << pi->getDeviceId() << std::endl
              << "# Friendly Name: " << pi->getFriendlyName() << std::endl
              << "# Nabto Version: " << pi->getNabtoVersion() << std::endl;
    if (!pi->getAppName().empty()) {
        std::cout << "# App Name     : " << pi->getAppName() << std::endl;
    }
    if (!pi->getAppVersion().empty()) {
        std::cout << "# App Version  : " << pi->getAppVersion() << std::endl;
    }
    std::cout << "# Pairing modes:" << std::endl;
    for (auto mode : ms) {
        std::cout << "# * " << IAM::pairingModeAsString(mode) << std::endl;
    }
}

int main1(int argc, char** argv)
{
    cxxopts::Options options("Tunnel client", "Nabto tunnel client example.");

    std::vector<std::string> services;

    options.add_options("General")
        ("h,help", "Show help")
        ("version", "Show version")
        ("H,home", "Override the directory in which configuration files are saved to.", cxxopts::value<std::string>())
        ("log-level", "Log level (none|error|info|trace)", cxxopts::value<std::string>()->default_value("error"))
        ;
    options.add_options("Bookmarks")
        ("bookmarks", "List bookmarked devices")
        ("b,bookmark", "Select a bookmarked device to use with other commands.", cxxopts::value<uint32_t>()->default_value("0"))
        ("delete-bookmark", "Delete a pairing with a device")
        ;

    options.add_options("Pairing")
        ("pair-local", "Pair the client with a tcp tunnel device running on the local network interactively")
        ("pair-string", "Pair with a tcp tunnel device using a pairing string", cxxopts::value<std::string>())
        ("pair-direct", "Pair with a tcp tunnel device directly using its ip or hostname", cxxopts::value<std::string>())
        ;

    options.add_options("IAM")
        ("users", "List all users on selected device.")
        ("roles", "List roles available on device.")
        ("get-me", "Get the user associated with the current connection.")
        ("get-user", "Get a user.")
        ("set-role", "Assign a role to a user.")
        ("delete-user", "Delete a user on device.")
        ("create-user", "Create a user new interactively in the device.")
        ("configure-open-pairing", "Configure pairing where users can register themselves in the device.")
        ("set-friendly-name", "Set the friendly name of the device.", cxxopts::value<std::string>())
        ("get-device-info","Get information about the device configuration")
        ;

    options.add_options("TCP Tunnelling")
        ("services", "List available services on the device")
        ("service", "Create a tunnel to this service. The default local port is an ephemeral port. A specific local port can be used using the syntax --service <service>:<port> e.g. --service ssh:4242 to establish a tunnel to the ssh service and listen for connections to it on the local TCP port 4242", cxxopts::value<std::vector<std::string> >(services))
        ;

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help"))
        {
            std::cout << options.help() << std::endl;
            PrintGeneralHelp();
            return 0;
        }

        if (result.count("version"))
        {
            std::cout << edge_tunnel_client_version() << " (SDK version " << nabto_client_version() << ")" << std::endl;
            return 0;
        }

        if (result.count("home")) {
            Configuration::makeDirectories(result["home"].as<std::string>());
        } else {
            Configuration::makeDirectories("");
        }

        std::string homeDir;
        if (result.count("home")) {
            homeDir = result["home"].as<std::string>();
        } else {
            homeDir = Configuration::getDefaultHomeDir();
        }

        Configuration::InitializeWithDirectory(homeDir);

        if (result.count("bookmarks"))
        {
            Configuration::PrintBookmarks();
            return 0;
        }

        if (result.count("delete-bookmark")) {
            if (!result.count("bookmark")) {
                std::cerr << "The argument --bookmark is required when deleting a bookmark." << std::endl;
            } else {
                Configuration::DeleteBookmark(result["bookmark"].as<uint32_t>());
            }
            return 0;
        }

        auto context = nabto::client::Context::create();

        context->setLogger(std::make_shared<MyLogger>());
        context->setLogLevel(result["log-level"].as<std::string>());

        if (result.count("pair-local")) {
            if (!interactive_pair(context)) {
                return 1;
            }
            return 0;
        }
        else if (result.count("pair-string")) {
            if (!string_pair(context, result["pair-string"].as<std::string>())) {
                return 1;
            }
            return 0;
        }
        else if (result.count("pair-direct")) {
            if (!direct_pair(context, result["pair-direct"].as<std::string>())) {
                return 1;
            }
            return 0;
        }

        else if (result.count("services") ||
                 result.count("service") ||
                 result.count("users") ||
                 result.count("roles") ||
                 result.count("set-role") ||
                 result.count("delete-user") ||
                 result.count("get-user") ||
                 result.count("get-me") ||
                 result.count("create-user") ||
                 result.count("configure-open-pairing") ||
                 result.count("set-friendly-name") ||
                 result.count("get-device-info"))

        {
            // For all these commands we need a paired device.
            uint32_t SelectedBookmark = result["bookmark"].as<uint32_t>();

            if (Configuration::HasNoBookmarks()) {
                std::cerr << "No devices have been paired, start by pairing the client with a device." << std::endl;
                return 1;
            }

            auto Device = Configuration::GetPairedDevice(SelectedBookmark);
            if (!Device)
            {
                std::cerr << "The bookmark " << SelectedBookmark << " does not exist" << std::endl;
                return 1;
            }

            auto connection = createConnection(context, *Device);
            if (!connection) {
                return 1;
            }
            std::cout << "Connected to the device " << Device->getFriendlyName() << std::endl;

            bool status = false;
            if (result.count("services")) {
                //status = list_services(connection);
            } else if (result.count("service")) {
                //status = tcptunnel(connection, services);
            } else if (result.count("users")) {
                status = IAM::list_users(connection);
            } else if (result.count("roles")) {
                status = IAM::list_roles(connection);
            } else if (result.count("set-role")) {
                status = IAM::set_role_interactive(connection);
            } else if (result.count("delete-user")) {
                status = IAM::delete_user_interactive(connection);
            } else if (result.count("get-user")) {
                status = IAM::get_user_interactive(connection);
            } else if (result.count("get-me")) {
                status = IAM::get_me_interactive(connection);
            } else if (result.count("create-user")) {
                status = IAM::create_user_interactive(connection);
            } else if (result.count("configure-open-pairing")) {
                status = IAM::configure_open_pairing_interactive(connection);
            } else if (result.count("set-friendly-name")) {
                auto ec = IAM::set_friendly_name(
                    connection, result["set-friendly-name"].as<std::string>());
                if (ec.ok()) {
                    std::cout << "Device successfully renamed to "
                              << result["set-friendly-name"].as<std::string>()
                              << std::endl;
                    status = true;
                } else {
                    ec.printError();
                    status = false;
                }
            } else if (result.count("get-device-info")) {
                IAM::IAMError ec; std::shared_ptr<IAM::PairingInfo> pi;
                std::tie(ec, pi) = IAM::get_pairing_info(connection);
                if (!ec.ok()) {
                    ec.printError();
                    status = false;
                } else {
                    printDeviceInfo(pi);
                    status = true;
                }
            }
            try {
                connection->close()->waitForResult();
            } catch(nabto::client::NabtoException& e) {
                if (e.status().getErrorCode() != nabto::client::Status::STOPPED) {
                    throw(e);
                }
            }
            return status ? 0 : 1;
        } else {
            std::cout << options.help() << std::endl;
            return 0;
        }
    } catch (std::exception& e) {
        std::cerr << "Invalid Option " << e.what() << std::endl;
        std::cout << options.help() << std::endl;
        return 1;
    }
}

class HttpServer {
public:
    HttpServer(int port) : serverPort(port) {}

    int initialize() {
        auto bookmarks = Configuration::PrintBookmarks();

        if (bookmarks.empty()) {
            std::cerr << "No bookmarks found." << std::endl;
            return 1;
        }

        // Access the first bookmark (arbitrary choice)
        auto firstBookmark = bookmarks.begin(); // Get the first element in the map
        std::cout << "Connecting to device with ID: " << firstBookmark->first << std::endl;

        auto device = Configuration::GetPairedDevice(firstBookmark->first);
        if (!device) {
            std::cerr << "Failed to retrieve device information for ID: " << firstBookmark->first << std::endl;
            return 1;
        }

        ctx = nabto::client::Context::create();
        connection = createConnection(ctx, *device);
        initializeEndpoints();
        return 0;
    }

    void start() {
        std::cout << "Server started at http://localhost:" << serverPort << "\n";
        server.listen("0.0.0.0", serverPort);
    }

private:
    httplib::Server server;
    int serverPort;
    std::mutex strMutex;
    std::shared_ptr<nabto::client::Context> ctx;
    std::map<int, Configuration::DeviceInfo> bookmarks;
    std::shared_ptr<nabto::client::Connection> connection;
    std::vector<std::shared_ptr<nabto::client::TcpTunnel>> tunnels;
    void initializeEndpoints() {
        server.Get("/devices", [this](const httplib::Request &req, httplib::Response &res) {
            handleGetDevices(req, res);
        });

        server.Get("/services", [this](const httplib::Request &req, httplib::Response &res) {
            handleGetServices(req, res);
        });

        server.Get("/connect", [this](const httplib::Request &req, httplib::Response &res) {
            handleConnect(req, res);
        });
    }

    void handleGetDevices(const httplib::Request &req, httplib::Response &res) {
        auto name = req.get_param_value("name");
        bookmarks = Configuration::PrintBookmarks();
        auto contex = nabto::client::Context::create();
        std::string str;
        std::cout << "name" + name << std::endl;
        auto connectToDevice = [&str, this, contex](const std::pair<int, Configuration::DeviceInfo>& bookmark) {
            try {
                auto d = Configuration::GetPairedDevice(bookmark.first);
                auto c = createConnection(contex, *d);
                if (c != nullptr) {
                    IAM::IAMError ec;
                    std::shared_ptr<IAM::PairingInfo> pi;
                    std::tie(ec, pi) = IAM::get_pairing_info(c);

                    if (pi) {
                        std::string localStr = pi->getFriendlyName() + ":" + bookmark.second.deviceId_ + "\n";
                        std::lock_guard<std::mutex> lock(strMutex);
                        str += localStr;
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(strMutex);
                str += "Failed to open a tunnel to " + bookmark.second.deviceId_ + ": " + e.what() + "\n";
            }
        };

        std::vector<std::future<void>> futures;
        for (const auto& bookmark : bookmarks) {
            futures.push_back(std::async(std::launch::async, connectToDevice, bookmark));
        }

        for (auto& future : futures) {
            future.get();
        }

        res.set_content(str, "text/plain");
    }


    void handleGetServices(const httplib::Request &req, httplib::Response &res) {
        std::string name = req.get_param_value("device");
        ctx = nabto::client::Context::create();
        std::string itemText;
        
        try {
            for (const auto& pair : bookmarks) {
                if (pair.second.deviceId_ == name) {
                    std::cout << " " << pair.first << " " << pair.second.deviceId_;
                    auto Device = Configuration::GetPairedDevice(pair.first);
                    if (!Device) {
                        std::cerr << "The bookmark does not exist" << std::endl;
                        continue;
                    }
                    connection = createConnection(ctx, *Device);
                    if (!connection) {
                        continue;
                    }

                    auto servs = list_services(connection);
                    tunnels.clear();
                    for (const auto& x : servs) {
                        itemText += x.first + ":" + (x.second.contains("Type") ? x.second["Type"].get<std::string>() : "Unknown") + ":" +(x.second.contains("Port") && x.second["Port"].is_number_integer() ? std::to_string(x.second["Port"].get<uint16_t>()) : "Unknown") + "\n";
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
        res.set_content(itemText, "text/plain");
    }

    void handleConnect(const httplib::Request &req, httplib::Response &res){
        std::string ser = req.get_param_value("service");
        std::vector<std::string> services;
        services.push_back(ser);
        std::cout << "Connecting to service: " << ser << std::endl;
        std::string string_tcptunnel = tcptunnel(connection, services);
        res.set_content(string_tcptunnel, "text/plain");
    }

    std::string tcptunnel(std::shared_ptr<nabto::client::Connection> connection, std::vector<std::string> services)
    {   
        std::string str="";
        for (auto serviceAndPort : services) {
            std::string service;
            uint16_t localPort;
            if (!split_in_service_and_port(serviceAndPort, service, localPort)) {
                return str;
            }
            
            std::shared_ptr<nabto::client::TcpTunnel> tunnel;
            try {
                tunnel = connection->createTcpTunnel();
                std::cout << serviceAndPort << std::endl;
                tunnel->open(service, localPort)->waitForResult();
                tunnels.push_back(tunnel);
            } catch (std::exception& e) {
                return "Failed to open a tunnel to " + serviceAndPort + " error: " + e.what();
            }

            str = "Tunnel opened for '" + service + "' -> local port " + std::to_string(tunnel->getLocalPort());
            
        }
        return str;
    }
};

int main() {
    int port = 8080;
    std::string homeDir = Configuration::getDefaultHomeDir();
    Configuration::InitializeWithDirectory(homeDir);

    HttpServer server(port);
    server.initialize();
    server.start();

    return 0;
}


