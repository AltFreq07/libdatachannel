/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "rtc/rtc.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;
using namespace chrono_literals;

using chrono::duration_cast;
using chrono::milliseconds;
using chrono::steady_clock;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

size_t benchmark(milliseconds duration) {
	rtc::InitLogger(LogLevel::Warning);
	rtc::Preload();

	Configuration config1;
	// config1.iceServers.emplace_back("stun:stun.l.google.com:19302");

	auto pc1 = std::make_shared<PeerConnection>(config1);

	Configuration config2;
	// config2.iceServers.emplace_back("stun:stun.l.google.com:19302");

	auto pc2 = std::make_shared<PeerConnection>(config2);

	pc1->onLocalDescription([wpc2 = make_weak_ptr(pc2)](const Description &sdp) {
		auto pc2 = wpc2.lock();
		if (!pc2)
			return;
		cout << "Description 1: " << sdp << endl;
		pc2->setRemoteDescription(sdp);
	});

	pc1->onLocalCandidate([wpc2 = make_weak_ptr(pc2)](const Candidate &candidate) {
		auto pc2 = wpc2.lock();
		if (!pc2)
			return;
		cout << "Candidate 1: " << candidate << endl;
		pc2->addRemoteCandidate(candidate);
	});

	pc1->onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });
	pc1->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
	});

	pc2->onLocalDescription([wpc1 = make_weak_ptr(pc1)](const Description &sdp) {
		auto pc1 = wpc1.lock();
		if (!pc1)
			return;
		cout << "Description 2: " << sdp << endl;
		pc1->setRemoteDescription(sdp);
	});

	pc2->onLocalCandidate([wpc1 = make_weak_ptr(pc1)](const Candidate &candidate) {
		auto pc1 = wpc1.lock();
		if (!pc1)
			return;
		cout << "Candidate 2: " << candidate << endl;
		pc1->addRemoteCandidate(candidate);
	});

	pc2->onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });
	pc2->onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	const size_t messageSize = 65535;
	binary messageData(messageSize);
	fill(messageData.begin(), messageData.end(), byte(0xFF));

	atomic<size_t> receivedSize = 0;
	atomic<bool> finished = false;

	steady_clock::time_point startTime, openTime, receivedTime, endTime;

	shared_ptr<DataChannel> dc2;
	pc2->onDataChannel(
	    [&dc2, &finished, &receivedSize, &receivedTime, &endTime](shared_ptr<DataChannel> dc) {
		    dc->onMessage([&receivedTime, &receivedSize](const variant<binary, string> &message) {
			    if (holds_alternative<binary>(message)) {
				    const auto &bin = get<binary>(message);
				    if (receivedSize == 0)
					    receivedTime = steady_clock::now();
				    receivedSize += bin.size();
			    }
		    });

		    dc->onClosed([&finished, &endTime]() {
			    cout << "DataChannel closed." << endl;
			    endTime = steady_clock::now();
			    finished = true;
		    });

		    std::atomic_store(&dc2, dc);
	    });

	startTime = steady_clock::now();
	auto dc1 = pc1->createDataChannel("benchmark");

	dc1->onOpen([wdc1 = make_weak_ptr(dc1), &messageData, &openTime]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		openTime = steady_clock::now();

		cout << "DataChannel open, sending data..." << endl;
		while (dc1->bufferedAmount() == 0) {
			dc1->send(messageData);
		}

		// When sent data is buffered in the DataChannel,
		// wait for onBufferedAmountLow callback to continue
	});

	dc1->onBufferedAmountLow([wdc1 = make_weak_ptr(dc1), &messageData]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		// Continue sending
		while (dc1->bufferedAmount() == 0) {
			dc1->send(messageData);
		}
	});

	const int steps = 10;
	const auto stepDuration = duration / 10;
	for (int i = 0; i < steps; ++i) {
		this_thread::sleep_for(stepDuration);
		cout << "Received: " << receivedSize.load() / 1000 << " KB" << endl;
	}

	if (auto adc2 = std::atomic_load(&dc2)) {
		dc1->close();
		while (!finished && adc2->isOpen())
			this_thread::sleep_for(100ms);
	}

	auto connectDuration = duration_cast<milliseconds>(openTime - startTime);
	auto transferDuration = duration_cast<milliseconds>(endTime - receivedTime);

	cout << "Test duration: " << duration.count() << " ms" << endl;
	cout << "Connect duration: " << connectDuration.count() << " ms" << endl;

	size_t received = receivedSize.load();
	size_t goodput = transferDuration.count() > 0 ? received / transferDuration.count() : 0;
	cout << "Goodput: " << goodput * 0.001 << " MB/s"
	     << " (" << goodput * 0.001 * 8 << " Mbit/s)" << endl;

	pc1->close();
	pc2->close();
	this_thread::sleep_for(1s);

	rtc::Cleanup();
	return goodput;
}

#ifdef BENCHMARK_MAIN
int main(int argc, char **argv) {
	try {
		size_t goodput = benchmark(30s);
		if (goodput == 0)
			throw runtime_error("No data received");

		return 0;

	} catch (const std::exception &e) {
		cerr << "Benchmark failed: " << e.what() << endl;
		return -1;
	}
}
#endif
