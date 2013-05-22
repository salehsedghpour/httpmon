#include <algorithm>
#include <array>
#include <boost/program_options.hpp>
#include <curl/curl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <thread>
#include <vector>

const int SpecialRecommendationMarker = 128;

typedef struct {
	std::string url;
	int timeout;
	volatile bool running;
	std::mutex mutex;
	uint32_t numErrors;
	uint32_t numRecommendations;
	std::vector<double> latencies;
} HttpClientControl;

double inline now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_usec / 1000000 + tv.tv_sec;
}

template<typename T>
typename T::value_type median(T begin, T end)
{
	/* assumes vector is sorted */
	int n = end - begin;

	if ((n-1) % 2 == 0)
		return *(begin+(n-1)/2);
	else
		return (*(begin+(n-1)/2) + *(begin+(n-1)/2+1))/2;
}

template<typename T>
std::array<typename T::value_type, 5> quartiles(T &a)
{
	/* return value: minimum, first quartile, median, third quartile, maximum */
	std::array<typename T::value_type, 5> ret;

	size_t n = a.size();
	if (n < 1)
		throw std::logic_error("No data to compute quartiles on");
	std::sort(a.begin(), a.end());

	ret[0] = a[0]  ; /* minimum */
	ret[4] = a[n-1]; /* maximum */

	ret[2] = median(a.begin(), a.end());
	ret[1] = median(a.begin(), a.begin() + n / 2);
	ret[3] = median(a.begin() + n / 2, a.end());

	return ret;
}

size_t nullWriter(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	bool *recommendation = (bool *)userdata;

	if (memchr(ptr, SpecialRecommendationMarker, size*nmemb) != NULL)
		*recommendation = true;
	return size * nmemb; /* i.e., pretend we are actually doing something */
}

int httpClientMain(int id, HttpClientControl &control)
{
	bool recommendation;

	CURL *curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_URL, control.url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullWriter);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullWriter);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, control.timeout);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &recommendation);

	while (control.running) {
		/* Send HTTP request */
		double start = now();
		recommendation = false;
		bool error = (curl_easy_perform(curl) != 0);
		double latency = now() - start;

		/* Add data to report */
		/* XXX: one day, this might be a bottleneck */
		{
			std::lock_guard<std::mutex> lock(control.mutex);
			if (error)
				control.numErrors ++;
			if (recommendation)
				control.numRecommendations ++;
			control.latencies.push_back(latency);
		}
	}
	curl_easy_cleanup(curl);

	return 0;
}

int main(int argc, char **argv)
{
	namespace po = boost::program_options;

	/*
	 * Initialize
	 */
	std::string url;
	int concurrency;
	int timeout;

	/*
	 * Parse command-line
	 */
	po::options_description desc("Real-time monitor of a HTTP server's throughput and latency");
	desc.add_options()
		("help", "produce help message")
		("url", po::value<std::string>(&url), "set URL to request")
		("concurrency", po::value<int>(&concurrency)->default_value(100), "set concurrency (number of HTTP client threads)")
		("timeout", po::value<int>(&timeout)->default_value(9), "set HTTP client timeout in seconds")
	;

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (vm.count("help")) {
		std::cout << desc << "\n";
		return 1;
	}

	/*
	 * Start HTTP client threads
	 */

	curl_global_init(CURL_GLOBAL_ALL);

	/* Setup thread control structure */
	HttpClientControl control;
	control.running = true;
	control.url = url;
	control.timeout = timeout;
	control.numErrors = 0;
	control.numRecommendations = 0;

	/* Start client threads */
	std::thread httpClientThreads[concurrency];
	for (int i = 0; i < concurrency; i++) {
		httpClientThreads[i] = std::thread(httpClientMain, i, std::ref(control));
	}

	/*
	 * Let client threads work, until user interrupts us
	 */

	/* Block SIGINT and SIGQUIT */
	sigset_t sigset;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGQUIT);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	/* Report at regular intervals */
	int signo;
	double lastReportTime = now();
	while (control.running) {
		struct timespec timeout = { 1, 0 };
		signo = sigtimedwait(&sigset, NULL, &timeout);

		if (signo > 0)
			control.running = false;

		int numErrors;
		int numRecommendations;
		std::vector<double> latencies;
		double reportTime;
		{
			std::lock_guard<std::mutex> lock(control.mutex);

			numErrors = control.numErrors;
			numRecommendations = control.numRecommendations;
			latencies = control.latencies;

			control.numErrors = 0;
			control.numRecommendations = 0;
			control.latencies.clear();
			reportTime = now();
		}

		int throughput = (double)latencies.size() / (reportTime - lastReportTime);
		int recommendationRate = numRecommendations * 100 / latencies.size();
		auto latencyQuartiles = quartiles(latencies);
		lastReportTime = reportTime;

		fprintf(stderr, "[%f] latency=%04d:%04d:%04d:%04d:%04dms throughput=%04drps rr=%02d%% errors=%04d\n",
			reportTime,
			int(latencyQuartiles[0] * 1000),
			int(latencyQuartiles[1] * 1000),
			int(latencyQuartiles[2] * 1000),
			int(latencyQuartiles[3] * 1000),
			int(latencyQuartiles[4] * 1000),
			throughput, recommendationRate, numErrors);
	}
	fprintf(stderr, "Got signal %d, cleaning up ...\n", signo);

	/*
	 * Cleanup
	 */
	for (int i = 0; i < concurrency; i++) {
		httpClientThreads[i].join();
	}
	curl_global_cleanup();

	return 0;
}
