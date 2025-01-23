#pragma once
#include <nabto_client.hpp>
#include <string>
#include <memory>
#include <set>

std::string interactive_pair(std::shared_ptr<nabto::client::Context> Context);
std::string string_pair(std::shared_ptr<nabto::client::Context> Context, const std::string& pairString, const std::string& user);
std::string direct_pair(std::shared_ptr<nabto::client::Context> Context, const std::string& host);

