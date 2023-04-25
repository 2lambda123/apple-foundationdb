/**
 * RKRateServer.h
 */

#include "fdbserver/IRKRateServer.h"
#include "fdbserver/IRKRateUpdater.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/TagThrottler.h"

class RKRateServerImpl {
public:
	ACTOR static Future<Void> run(RKRateServer* self,
	                              IRKRateUpdater const* normalRateUpdater,
	                              IRKRateUpdater const* batchRateUpdater,
	                              ITagThrottler* tagThrottler,
	                              IRKRecoveryTracker* recoveryTracker) {
		loop {
			GetRateInfoRequest req = waitNext(self->getRateInfo);
			GetRateInfoReply reply;

			auto& p = self->grvProxyInfo[req.requesterID];
			//TraceEvent("RKMPU", req.requesterID).detail("TRT", req.totalReleasedTransactions).detail("Last", p.totalTransactions).detail("Delta", req.totalReleasedTransactions - p.totalTransactions);
			if (p.totalTransactions > 0) {
				self->smoothReleasedTransactions.addDelta(req.totalReleasedTransactions - p.totalTransactions);

				for (auto const& [tag, count] : req.throttledTagCounts) {
					tagThrottler->addRequests(tag, count);
				}
			}
			if (p.batchTransactions > 0) {
				self->smoothBatchReleasedTransactions.addDelta(req.batchReleasedTransactions - p.batchTransactions);
			}

			p.totalTransactions = req.totalReleasedTransactions;
			p.batchTransactions = req.batchReleasedTransactions;
			p.version = req.version;

			recoveryTracker->updateMaxVersion(req.version);

			p.lastUpdateTime = now();

			reply.transactionRate = normalRateUpdater->getTpsLimit() / self->grvProxyInfo.size();
			reply.batchTransactionRate = batchRateUpdater->getTpsLimit() / self->grvProxyInfo.size();
			reply.leaseDuration = SERVER_KNOBS->METRIC_UPDATE_RATE;

			if (p.lastThrottledTagChangeId != tagThrottler->getThrottledTagChangeId() ||
			    now() > p.lastTagPushTime + SERVER_KNOBS->TAG_THROTTLE_PUSH_INTERVAL) {
				p.lastThrottledTagChangeId = tagThrottler->getThrottledTagChangeId();
				p.lastTagPushTime = now();

				bool returningTagsToProxy{ false };
				if (SERVER_KNOBS->ENFORCE_TAG_THROTTLING_ON_PROXIES) {
					auto proxyThrottledTags = tagThrottler->getProxyRates(self->grvProxyInfo.size());
					if (!SERVER_KNOBS->GLOBAL_TAG_THROTTLING_REPORT_ONLY) {
						returningTagsToProxy = proxyThrottledTags.size() > 0;
						reply.proxyThrottledTags = std::move(proxyThrottledTags);
					}
				} else {
					auto clientThrottledTags = tagThrottler->getClientRates();
					if (!SERVER_KNOBS->GLOBAL_TAG_THROTTLING_REPORT_ONLY) {
						returningTagsToProxy = clientThrottledTags.size() > 0;
						reply.clientThrottledTags = std::move(clientThrottledTags);
					}
				}
				CODE_PROBE(returningTagsToProxy, "Returning tag throttles to a proxy");
			}

			reply.healthMetrics.update(normalRateUpdater->getHealthMetrics(), true, req.detailed);
			reply.healthMetrics.tpsLimit = normalRateUpdater->getTpsLimit();
			reply.healthMetrics.batchLimited = self->lastLimited;

			req.reply.send(reply);
		}
	}
}; // class RKRateServerImpl

RKRateServer::RKRateServer(FutureStream<GetRateInfoRequest> getRateInfo)
  : getRateInfo(getRateInfo), smoothReleasedTransactions(SERVER_KNOBS->SMOOTHING_AMOUNT),
    smoothBatchReleasedTransactions(SERVER_KNOBS->SMOOTHING_AMOUNT) {}

RKRateServer::~RKRateServer() = default;

double RKRateServer::getSmoothReleasedTransactionRate() const {
	return smoothReleasedTransactions.smoothRate();
}

double RKRateServer::getSmoothBatchReleasedTransactionRate() const {
	return smoothBatchReleasedTransactions.smoothRate();
}

std::unordered_map<UID, RKGrvProxyInfo> const& RKRateServer::getGrvProxyInfo() const& {
	return grvProxyInfo;
}

void RKRateServer::cleanupExpiredGrvProxies() {
	double tooOld = now() - 1.0;
	for (auto p = grvProxyInfo.begin(); p != grvProxyInfo.end();) {
		if (p->second.lastUpdateTime < tooOld)
			p = grvProxyInfo.erase(p);
		else
			++p;
	}
}

void RKRateServer::updateLastLimited(double batchTpsLimit) {
	lastLimited = getSmoothReleasedTransactionRate() > SERVER_KNOBS->LAST_LIMITED_RATIO * batchTpsLimit;
}

Future<Void> RKRateServer::run(IRKRateUpdater const& normalRateUpdater,
                               IRKRateUpdater const& batchRateUpdater,
                               ITagThrottler& tagThrottler,
                               IRKRecoveryTracker& recoveryTracker) {
	return RKRateServerImpl::run(this, &normalRateUpdater, &batchRateUpdater, &tagThrottler, &recoveryTracker);
}
