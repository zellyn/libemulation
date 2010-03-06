
/**
 * libemulator
 * MC6821
 * (C) 2010 by Marc S. Ressl (mressl@umich.edu)
 * Released under the GPL
 *
 * Controls a generic MC6821 Peripheral Interface Adapter
 */

#include "MC6821.h"

MC6821::MC6821()
{
	reset();
}

void MC6821::reset()
{
	controlRegisterA = 0;
	dataDirectionRegisterA = 0;
	dataRegisterA = 0;
	controlRegisterB = 0;
	dataDirectionRegisterB = 0;
	dataRegisterB = 0;
}

int MC6821::ioctl(int message, void *data)
{
	switch(message)
	{
		case OEIOCTL_CONNECT:
		{
			OEIoctlConnection *connection = (OEIoctlConnection *) data;
			if (connection->name == "interfaceA")
				interfaceA = connection->component;
			else if (connection->name == "interfaceB")
				interfaceB = connection->component;
			else if (connection->name == "irqA")
				irqA = connection->component;
			else if (connection->name == "irqB")
				irqB = connection->component;
			break;
		}
		case MC6821_RESET:
		{
			reset();
			break;
		}
	}
	
	return 0;
}

int MC6821::read(int address)
{
	switch(address & 0x3)
	{
		case MC6821_RS_DATAREGISTERA:
			if (controlRegisterA & MC6821_CR_DATAREGISTER)
			{
				if (controlRegisterA)
					break;
				return dataRegisterA;
			}
			else
				return dataDirectionRegisterA;
		case MC6821_RS_CONTROLREGISTERA:
			return controlRegisterA;
		case MC6821_RS_DATAREGISTERB:
			if (controlRegisterB & MC6821_CR_DATAREGISTER)
				return dataRegisterB;
			else
				return dataDirectionRegisterB;
		case MC6821_RS_CONTROLREGISTERB:
			return controlRegisterB;
	}
	
	return 0;
}

void MC6821::write(int address, int value)
{
	switch(address & 0x3)
	{
		case MC6821_RS_DATAREGISTERA:
		case MC6821_RS_CONTROLREGISTERA:
		case MC6821_RS_DATAREGISTERB:
		case MC6821_RS_CONTROLREGISTERB:
			break;
	}
}