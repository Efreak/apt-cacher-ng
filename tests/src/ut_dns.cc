#include "gtest/gtest.h"
#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"
#include "acngbase.h"
#include "sockio.h"
#include <dns/caddrinfo.h>

#include "gmock/gmock.h"

#include <unordered_map>

using namespace acng;

TEST(dns, hostNameBad)
{
	auto rc = CreateRegularSystemResources();
	CAddrInfoPtr dnsresult;
	rc->dnsbase->Resolve("this.does.not.exist.localhost", "12345", [&](CAddrInfoPtr ainfo){
		dnsresult = ainfo;
		rc->fore->StartShutdown();
	});
	int status = rc->MainLoop();
	ASSERT_EQ(status, 0);
	ASSERT_TRUE(dnsresult);
	ASSERT_FALSE(dnsresult->getError().empty());
	ASSERT_TRUE(startsWithSz(dnsresult->getError(), "503"));
	std::cout << dnsresult->getError() << std::endl;
}

TEST(dns, hostNameGood)
{
	auto rc = CreateRegularSystemResources();
	CAddrInfoPtr dnsresult;
	rc->dnsbase->Resolve("localhost", "12345", [&](CAddrInfoPtr ainfo){
		dnsresult = ainfo;
		rc->fore->StartShutdown();
	});
	int status = rc->MainLoop();
	ASSERT_EQ(status, 0);
	ASSERT_TRUE(dnsresult);
	ASSERT_TRUE(dnsresult->getError().empty());
	auto result = formatIpPort(dnsresult->getTcpAddrInfo());
	//std::cout << result << std::endl;
	ASSERT_TRUE(result.length() > 10);
	ASSERT_EQ(result.substr(0, 3), "127");
}

TEST(dns, hostNameMultipleInARow)
{
	auto rc = CreateRegularSystemResources();
	std::vector<CAddrInfoPtr> dnsresults;
	rc->dnsbase->Resolve("this.does.not.exist.localhost", "12345", [&](CAddrInfoPtr ainfo){
		dnsresults.emplace_back(move(ainfo));
	});
	rc->dnsbase->Resolve("this.does.not.exist.localhost", "12345", [&](CAddrInfoPtr ainfo){
		dnsresults.emplace_back(move(ainfo));
	});
	rc->dnsbase->Resolve("this.does.not.exist.localhost", "12345", [&](CAddrInfoPtr ainfo){
		dnsresults.emplace_back(move(ainfo));
		rc->fore->StartShutdown();
	});
	int status = rc->MainLoop();
	ASSERT_EQ(status, 0);
	ASSERT_EQ(3, dnsresults.size());
	for(auto& el: dnsresults)
		ASSERT_EQ(el, dnsresults.front());
}

