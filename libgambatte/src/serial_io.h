//
//   Copyright (C) 2007 by sinamas <sinamas at users.sourceforge.net>
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//
//   You should have received a copy of the GNU General Public License
//   version 2 along with this program; if not, write to the
//   Free Software Foundation, Inc.,
//   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//

#ifndef SERIAL_IO_H
#define SERIAL_IO_H

namespace gambatte {

class SerialIO
{
	public:
		virtual ~SerialIO() {};

		virtual bool check(unsigned char out, unsigned char& in, bool& fastCgb) = 0;
		virtual unsigned char send(unsigned char data, bool fastCgb) = 0;

		/* Asynchronous transfers on the internal clock (optional): when
		 * async() is true, send() only starts the transfer (what it returns
		 * is not used) and the game runs on while the other end answers;
		 * reply() gives the byte that came back once it is there (it may
		 * also give up and give FF). cancel(): the game stopped waiting. */
		virtual bool async() const { return false; }
		virtual bool reply(unsigned char &in) { (void)in; return false; }
		virtual void cancel() {}
		/* Asynchronous cables only, often (while no transfer of ours
		 * waits): whether a transfer on the external clock is ready. */
		virtual void idle(bool waiting) { (void)waiting; }
};

/* A routine of the game that exchanges a block over the cable, byte by byte
 * (Pokémon's Serial_ExchangeBytes: "send bc bytes from hl, receive bc bytes
 * to de"), done at once instead (see GB::setBlockHook). */
class BlockExchanger
{
	public:
		enum { RUN, WAIT, DONE };
		virtual ~BlockExchanger() {}
		/* send: the bytes the game sends (len of them). RUN: let the game
		 * run its routine; WAIT: not yet (the Game Boy stops until next
		 * time); DONE: recv holds the len bytes the game receives. */
		virtual int exchange(const unsigned char *send, unsigned len,
				unsigned char *recv) = 0;
};

}

#endif
