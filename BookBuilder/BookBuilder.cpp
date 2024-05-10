#include <libwebsockets.h>
#include <rapidjson/document.h>
#include <string>
#include <signal.h>
#include <chrono>
#include <ev.h>
#include <liburing.h>
#include "./OrderBook/OrderBook.hpp"
#include "./ThroughputMonitor/ThroughputMonitor.hpp"
#include "../SPSCQueue/SPSCQueue.hpp"
#include "../Utils/Utils.hpp"

#define MAX_JSON_SIZE 8192
#define CPU_CORE_NUMBER_OFFSET_FOR_BOOK_BUILDER_THREAD 2
#define WEBSOCKET_CLIENT_RX_BUF_SIZE 1024
#define NUMBER_OF_IO_URING_SQ_ENTRIES 8

using namespace rapidjson;
using namespace std::chrono;
using Clock = std::chrono::high_resolution_clock;

std::unordered_map<std::string, OrderBook> orderBookMap;
SPSCQueue<OrderBook>* bookBuilderToStrategyQueue = nullptr;
ThroughputMonitor* updateThroughputMonitor = nullptr; 
/*const std::vector<std::string> currencyPairs = {"BTCUSD", "ETHBTC", "ETHUSD"};*/
const std::vector<std::string> currencyPairs = {"XBTETH", "XBTUSDT", "ETHUSDT"};
// const std::vector<std::string> currencyPairs = {"XBTUSD", "ETHUSD", "XBTETH", "XBTUSDT", "SOLUSD", "XRPUSD", "LINKUSD", "SOLUSD", "XRPUSD"};
// const std::vector<std::string> currencyPairs = {"XBTUSD", "ETHUSD", "SOLUSD", "XBTUSDT", "DOGEUSD", "XBTH24", "LINKUSD", "XRPUSD", "SOLUSDT", "BCHUSD"};

static int interrupted, rx_seen, test;
static struct lws *client_wsi;

static struct ev_loop *loop_ev; 
ev_io socket_watcher;
ev_timer timeout_watcher;

SSL *ssl;
BIO *rbio;
int sockfd;

struct io_uring ring;
struct io_uring_sqe *sqe;
struct io_uring_cqe *cqe;

static char partial_ob_json_buffer[MAX_JSON_SIZE];
static size_t partial_ob_json_buffer_len = 0;

// static const struct lws_extension extensions[] = {
//         {
//                 "permessage-deflate",
//                 lws_extension_callback_pm_deflate,
//                       "permessage-deflate"
//                       "; client_no_context_takeover"
//                       "; client_max_window_bits"
//         },
//         { NULL, NULL, NULL /* terminator */ }
// };

static int
book_builder_lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    struct my_conn *mco = (struct my_conn *)user;
    uint64_t latency_us, now_us;
    const char *p;
    size_t alen;
    Document doc;
    GenericValue<rapidjson::UTF8<>>::MemberIterator data;
    const char* symbol; 
    uint64_t id;
    const char* action;
    const char* side;
    double size;
    double price;
    const char* timestamp;
    system_clock::time_point marketUpdateReceiveTimestamp;
    long exchangeUpdateTimestamp;
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            lwsl_err("CLIENT_CONNECTION_ERROR: %s\n",
                in ? (char *)in : "(null)");
            client_wsi = NULL;
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            /*
             * The messages are a few 100 bytes of JSON each
             */

            // lwsl_hexdump_notice(in, len);

            updateThroughputMonitor->operationCompleted();

            marketUpdateReceiveTimestamp = high_resolution_clock::now();
            /*std::cout << updateExchangeTimestamp << std::endl;*/
            // auto networkLatency = std::chrono::duration_cast<std::chrono::microseconds>(programReceiveTime - exchangeUnixTimestamp);

            // printf("\nReceived size: %ld, %d, JSON: %s\n", len, (int)len, (const char *)in);

            if (((const char *)in)[0] == '{' && ((const char *)in)[len - 1] == '}') {
                if (((const char *)in)[2] == 'i' || ((const char *)in)[2] == 's') {
                    break;
                }

                // std::cout << "CASE 1 HIT" << std::endl;
                doc.Parse((const char *)in, len);
            } else if (((const char *)in)[0] == '{') {
                // std::cout << "CASE 2 HIT" << std::endl;
                memset(partial_ob_json_buffer, 0, MAX_JSON_SIZE);
                partial_ob_json_buffer_len = 0;

                memcpy(partial_ob_json_buffer + partial_ob_json_buffer_len, in, len);
                partial_ob_json_buffer_len += len;
                break;
            } else if (((const char *)in)[len - 1] == '}') {
                // std::cout << "CASE 3 HIT" << std::endl;
                memcpy(partial_ob_json_buffer + partial_ob_json_buffer_len, in, len);
                partial_ob_json_buffer_len += len;

                doc.Parse(partial_ob_json_buffer, partial_ob_json_buffer_len);
            } else {
                // std::cout << "CASE 4 HIT" << std::endl;
                memcpy(partial_ob_json_buffer + partial_ob_json_buffer_len, in, len);
                partial_ob_json_buffer_len += len;
                break;
            }

            if (doc.HasParseError()) {
                lwsl_err("%s: no E JSON\n", __func__);
                std::cerr << "Error parsing JSON: " << doc.GetParseError() << std::endl;
                break;
            }

            data = doc.FindMember("data");
            action = doc.FindMember("action")->value.GetString();

            for (SizeType i = 0; i < data->value.Size(); i++) {
                symbol = data->value[i].FindMember("symbol")->value.GetString();
                id = data->value[i].FindMember("id")->value.GetInt64();
                side = data->value[i].FindMember("side")->value.GetString();

                if (data->value[i].HasMember("size")) 
                    size = data->value[i].FindMember("size")->value.GetInt64();
                
                price = data->value[i].FindMember("price")->value.GetDouble();
                timestamp = data->value[i].FindMember("timestamp")->value.GetString();
                exchangeUpdateTimestamp = convertTimestampToTimePoint(timestamp);

                std::string sideStr(side);

                if (sideStr == "Buy") {
                    switch (action[0]) {
                        case 'p':
                        case 'i':
                            orderBookMap[symbol].insertBuy(id, price, size, exchangeUpdateTimestamp, marketUpdateReceiveTimestamp);
                            break;
                        case 'u':
                            orderBookMap[symbol].updateBuy(id, size, exchangeUpdateTimestamp, marketUpdateReceiveTimestamp);
                            break;
                        case 'd':
                            orderBookMap[symbol].removeBuy(id, exchangeUpdateTimestamp, marketUpdateReceiveTimestamp);
                            break;
                        default:
                            break;
                    }
                } else if (sideStr == "Sell") {
                    switch (action[0]) {
                        case 'p':
                        case 'i':
                            orderBookMap[symbol].insertSell(id, price, size, exchangeUpdateTimestamp, marketUpdateReceiveTimestamp);
                            break;
                        case 'u':
                            orderBookMap[symbol].updateSell(id, size, exchangeUpdateTimestamp, marketUpdateReceiveTimestamp);
                            break;
                        case 'd':
                            orderBookMap[symbol].removeSell(id, exchangeUpdateTimestamp, marketUpdateReceiveTimestamp);
                            break;
                        default:
                            break;
                    }
                }
                
                while (!bookBuilderToStrategyQueue->push(orderBookMap[symbol]));

                // auto bookBuildingEndTime = Clock::now();
                // auto bookBuildingDuration = std::chrono::duration_cast<std::chrono::microseconds>(bookBuildingEndTime - programReceiveTime);
                // std::cout << "Symbol: " << symbol << " - Action: " << action << " - Size: " << size << " - Price: " << price << " (" << side << ")" << " - id: " << id << " - Timestamp: " << updateExchangeTimestamp << std::endl;
                // std::cout << "Time taken to receive market update: " << networkLatency.count() << " microseconds" << std::endl;
                // std::cout << "Time taken to process market update: " << bookBuildingDuration.count() << " microseconds" << std::endl;
                // std::cout << "Timestamp: " << updateExchangeTimestamp << std::endl;
                // orderBook.printOrderBook();
                // orderBook.updateOrderBookMemoryUsage();
            }

            break;

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            lwsl_user("%s: established\n", __func__);

            // Send subscription message here
            for (const std::string& currencyPair : currencyPairs) { 
                    std::string subscriptionMessage = "{\"op\":\"subscribe\",\"args\":[\"orderBookL2_25:" + currencyPair + "\"]}";
                    // Allocate buffer with LWS_PRE bytes before the data
                    unsigned char buf[LWS_PRE + subscriptionMessage.size()];

                    // Copy subscription message to buffer after LWS_PRE bytes
                    memcpy(&buf[LWS_PRE], subscriptionMessage.c_str(), subscriptionMessage.size());
                    
                    // Send data using lws_write
                    lws_write(wsi, &buf[LWS_PRE], subscriptionMessage.size(), LWS_WRITE_TEXT);
            }
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            client_wsi = NULL;
            break;

        default:
            break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static const struct lws_protocols protocols[] = {
        { "book-builder-lws-client", book_builder_lws_callback, 0, 0, 0, NULL, 0 },
        LWS_PROTOCOL_LIST_TERM
};

static void
sigint_handler(int sig)
{
    interrupted = 1;
}

static void socket_cb (EV_P_ ev_io *w, int revents) {
    if (revents & EV_READ) { 
        puts ("SOCKET ready for reading\n");
		// int n;	
		int k;

		do {
            char buffer[16384 + LWS_PRE];	
			char *px = buffer + LWS_PRE;
			int lenx = sizeof(buffer) - LWS_PRE;
            printf("%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%");
            // n = SSL_read(ssl, &px, lenx);
			
			sqe = io_uring_get_sqe(&ring);

            if (!sqe) {
                perror("io_uring_get_sqe");
                io_uring_queue_exit(&ring);
                return;
            }
			
			printf("Reading for sockfd %d\n", sockfd);
			io_uring_prep_read(sqe, sockfd, px, lenx, 0);  // use offset on same buffer later?

			if (io_uring_submit(&ring) < 0) {
				perror("io_uring_submit");
				io_uring_queue_exit(&ring);
				return;
			}

			int ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret < 0) {
                perror("Error waiting for completion: %s\n");
                return;
            }

            // Read response
            if (cqe->res <= 0) {
                perror("io_uring completion error");
                return;
            } 

			int bytes_read = cqe->res;
				

            io_uring_cqe_seen(&ring, cqe);

			// int bytes_read = read(sockfd, px, lenx);
			// printf("buffer: %s, bytes read: %d\n\n", buffer, bytes_read);

			int n = BIO_write(rbio, px, bytes_read);
			// // printf("ZZZ bytes BIO written: %d\n", k);

			memset(buffer, 0, sizeof(buffer));

			k = SSL_read(ssl, &px, lenx);
			// printf("KKK buffer: %s, bytes read: %d\n\n", buffer, bytes_read);

			printf("BYTES READ: %d\n", k);
            if (k > 0) {
                printf("%s\n", buffer);
            }
        } while (k > 0);
    }
}

// another callback, this time for a time-out
static void
timeout_cb (EV_P_ ev_timer *w, int revents)
{
  puts ("timeout");
  // this causes the innermost ev_run to stop iterating
  ev_break (EV_A_ EVBREAK_ONE);
}

void bookBuilder(SPSCQueue<OrderBook>& bookBuilderToStrategyQueue_) {
    int numCores = std::thread::hardware_concurrency();
    
    if (numCores == 0) {
        std::cerr << "Error: Unable to determine the number of CPU cores." << std::endl;
        return;
    }

    int cpuCoreNumberForBookBuilderThread = numCores - CPU_CORE_NUMBER_OFFSET_FOR_BOOK_BUILDER_THREAD;
    setThreadAffinity(pthread_self(), cpuCoreNumberForBookBuilderThread);

    // Set the current thread's real-time priority to highest value
    struct sched_param schedParams;
    schedParams.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &schedParams);

    for (std::string currencyPair : currencyPairs) { 
        orderBookMap[currencyPair] = OrderBook(currencyPair);
    }

    bookBuilderToStrategyQueue = &bookBuilderToStrategyQueue_;

    ThroughputMonitor updateThroughputMonitorBookBuilder("Book Builder Throughput Monitor", std::chrono::high_resolution_clock::now());
    updateThroughputMonitor = &updateThroughputMonitorBookBuilder;

    int ret = io_uring_queue_init(NUMBER_OF_IO_URING_SQ_ENTRIES, &ring, 0);
    if (ret) {
        perror("io_uring_queue_init");
        return;
    }

    struct lws_context_creation_info info;
	struct lws_client_connect_info i;
	struct lws_context *context;
	const char *p;
	int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE
		/* for LLL_ verbosity above NOTICE to be built into lws, lws
		 * must have been configured with -DCMAKE_BUILD_TYPE=DEBUG
		 * instead of =RELEASE */
		/* | LLL_INFO */ /* | LLL_PARSER */ /* | LLL_HEADER */
		/* | LLL_EXT */ /* | LLL_CLIENT */ /* | LLL_LATENCY */
		/* | LLL_DEBUG */;

	signal(SIGINT, sigint_handler);
	// if ((p = lws_cmdline_option(argc, argv, "-d")))
	// 	logs = atoi(p);

	// test = !!lws_cmdline_option(argc, argv, "-t");

	lws_set_log_level(logs, NULL);
	lwsl_user("LWS Book Builder ws client rx [-d <logs>] [--h2] [-t (test)]\n");

	memset(&info, 0, sizeof info);
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_WITH_LIBEV;;
	info.port = CONTEXT_PORT_NO_LISTEN; 
	info.protocols = protocols;

    loop_ev = ev_default_loop(EVBACKEND_EPOLL);
    void *foreign_loops[1];
    foreign_loops[0] = loop_ev;
    info.foreign_loops = foreign_loops;

	info.fd_limit_per_thread = 1 + 1 + 1;

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return;
	}

	memset(&i, 0, sizeof i);
	i.context = context;
	i.port = 443;
	i.address = "ws.testnet.bitmex.com";
	// i.path = "/realtime?subscribe=orderBookL2_25:XBTUSDT";
    i.path = "/realtime";
	i.host = i.address;
	i.origin = i.address;
	i.ssl_connection = LCCSCF_USE_SSL | LCCSCF_PRIORITIZE_READS;
	i.protocol = NULL; 
	i.pwsi = &client_wsi;

	lws_client_connect_via_info(&i);

	while (n >= 0 && client_wsi && !interrupted)
		n = lws_service(context, 0);

    sockfd = lws_get_socket_fd(client_wsi);
	ssl = lws_get_ssl(client_wsi);
	rbio = BIO_new(BIO_s_mem());
	SSL_set_bio(ssl, rbio, NULL);

    ev_io_init (&socket_watcher, socket_cb, lws_get_socket_fd(client_wsi), EV_READ);
    ev_io_start (loop_ev, &socket_watcher);
    
    // initialise a timer watcher, then start it
    // simple non-repeating 5.5 second timeout
    ev_timer_init (&timeout_watcher, timeout_cb, 5.5, 0.);
    ev_timer_start (loop_ev, &timeout_watcher);

    ev_run (loop_ev, 0);

	lws_context_destroy(context);    
}