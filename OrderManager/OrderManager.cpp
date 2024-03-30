#include "OrderManager.hpp"

using json = nlohmann::json;
using namespace std::chrono;

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total_size = size * nmemb;
    output->append((char*)contents, total_size);
    return total_size;
}

std::string CalcHmacSHA256(std::string_view decodedKey, std::string_view msg)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned int hashLen;

    HMAC(
        EVP_sha256(),
        decodedKey.data(),
        static_cast<int>(decodedKey.size()),
        reinterpret_cast<unsigned char const*>(msg.data()),
        static_cast<int>(msg.size()),
        hash.data(),
        &hashLen
    );

    return std::string{reinterpret_cast<char const*>(hash.data()), hashLen};
}

std::string toHex(const std::string& input) {
    std::ostringstream hexStream;
    hexStream << std::hex << std::setfill('0');
    for (unsigned char c : input) {
        hexStream << std::setw(2) << static_cast<int>(c);
    }
    return hexStream.str();
}

void orderManager(int cpu, SPSCQueue<std::string>& strategyToOrderManagerQueue) {
    pinThread(cpu);

    CURL* curl;
    CURLcode res;
    std::string apiKey = "63ObNQpYqaCVrjTuBbhgFm2p";
    std::string apiSecret = "D2OBzpfW-i6FfgmqGnrhpYqKPrxCvIYnu5KZKsZQW_09XkF-";

    curl_global_init(CURL_GLOBAL_DEFAULT);

    curl = curl_easy_init();
    if (curl) {
        // Set the BitMEX API endpoint
        curl_easy_setopt(curl, CURLOPT_URL, "https://testnet.bitmex.com/api/v1/order");

        while (true) {
            // Set the necessary POST data
            std::string data;
            while (!strategyToOrderManagerQueue.pop(data));


            std::string orderData = data.substr(0, data.length() - 39);
            std::string updateExchangeTimepoint = data.substr(data.length() - 39, 13);
            std::string updateReceiveTimepoint = data.substr(data.length() - 26, 13);
            std::string strategyTimepoint = data.substr(data.length() - 13);

            /*std::cout << "Order data: " << orderData << std::endl;
            std::cout << "Strategy timepoint: " << strategyTimepoint << std::endl;
            std::cout << "Update Exchange timepoint: " << updateExchangeTimepoint << std::endl;*/
            // Get the current time_point
            std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
            // Add 1 hour to the current time_point
            std::chrono::system_clock::time_point oneHourLater = now + std::chrono::hours(1);
            // Convert the time_point to a Unix timestamp
            std::time_t timestamp = std::chrono::system_clock::to_time_t(oneHourLater);
            // Convert the timestamp to a string
            std::string expires = std::to_string(timestamp);
            // std::cout << "expires: " << expires << std::endl;
            std::string verb = "POST";
            std::string path = "/api/v1/order";
            
            // Concatenate the string to be hashed
            std::string concatenatedString = verb + path + expires + orderData;
            // Calculate HMAC-SHA256
            std::string signature = CalcHmacSHA256(apiSecret, concatenatedString);
            std::string hexSignature = toHex(signature);        
            // Print the hexSignature
            // std::cout << "Signature: " << hexSignature << std::endl;
            // Build the headers
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, ("api-key: " + apiKey).c_str());
            headers = curl_slist_append(headers, ("api-expires: " + expires).c_str());
            headers = curl_slist_append(headers, ("api-signature: " + hexSignature).c_str());
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            // Set the POST data and other options
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, orderData.c_str());
            system_clock::time_point submissionTimestamp = high_resolution_clock::now();
            std::string submissionTimepoint = std::to_string(duration_cast<milliseconds>(submissionTimestamp.time_since_epoch()).count());
            // Declare 'response' here
            std::string response;
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            // Perform the request
            res = curl_easy_perform(curl);
            // Check for errors
            if (res != CURLE_OK)
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            else {
                json jsonResponse = json::parse(response);
                // Access the "orderID" field

                if (jsonResponse["error"]["name"] == "RateLimitError") {
                    std::this_thread::sleep_for(milliseconds(1500));
                    continue;
                }

                long exchangeExecutionTimestamp = convertTimestampToTimePoint(jsonResponse["timestamp"]);

                // Do something with the orderID

                std::cout
                          << "===========================================================================================\n"
                          << "NEW ORDER EXECUTED\n"
                          << "Exchange to Receival (ms): " << getTimeDifferenceInMillis(updateExchangeTimepoint, updateReceiveTimepoint) << "      "
                          << "Receival to Detection (ms): " << getTimeDifferenceInMillis(updateReceiveTimepoint, strategyTimepoint) << "      "
                          << "Detection to Submission (ms): " << getTimeDifferenceInMillis(strategyTimepoint, submissionTimepoint) << "      "
                          << "Submission to Execution (ms): " << getTimeDifferenceInMillis(submissionTimepoint, std::to_string(exchangeExecutionTimestamp)) << "      "
                          << "Total Latency: " << getTimeDifferenceInMillis(updateExchangeTimepoint, std::to_string(exchangeExecutionTimestamp)) << "      "
                          << "Sync Diff: " << getTimeDifferenceInMillis(updateExchangeTimepoint, std::to_string(exchangeExecutionTimestamp)) - (
                                                                                            getTimeDifferenceInMillis(strategyTimepoint, std::to_string(exchangeExecutionTimestamp))
                                                                                                + getTimeDifferenceInMillis(updateExchangeTimepoint, strategyTimepoint)) << "      \n"

                          << "Update Exch. Ts.: " << updateExchangeTimepoint << "      "
                          << "Update Rec. Ts.: " << updateReceiveTimepoint << "      "
                          << "Strat. Ts.: " << strategyTimepoint << "      "
                          << "Submission. Ts.: " << submissionTimepoint << "      "
                          << "Execution. Ts.: " << exchangeExecutionTimestamp << "      \n"
                          << "\nResponse from exchange:\n" << response
                          << std::endl;
            }
            // Cleanup
            curl_slist_free_all(headers);
        }
        
        curl_easy_cleanup(curl);
    }    

    curl_global_cleanup();
}
