#ifndef SOURCE_JSONSTATS_H_
#define SOURCE_JSONSTATS_H_

#include <expiration.h>

class jsonstats: public expiration
{
public:
	jsonstats(const tRunParms & parms);// : public expiration(parms) {};
	virtual ~jsonstats();
	void Run() override;
	void Action() override;
protected:
	void PrintJson();
};

#endif /* SOURCE_JSONSTATS_H_ */
