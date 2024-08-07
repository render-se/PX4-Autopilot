/****************************************************************************
 *
 *   Copyright (c) 2013-2015 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file px4io_serial.cpp
 *
 * Serial interface for PX4IO on STM32F7
 */

#include <syslog.h>

#include <px4_arch/px4io_serial.h>

#include "stm32_uart.h"
#include <nuttx/cache.h>

/* serial register accessors */
#define REG(_x)		(*(volatile uint32_t *)(PX4IO_SERIAL_BASE + (_x)))
#define rISR		REG(STM32_USART_ISR_OFFSET)
#define rISR_ERR_FLAGS_MASK (0x1f)
#define rICR		REG(STM32_USART_ICR_OFFSET)
#define rRDR		REG(STM32_USART_RDR_OFFSET)
#define rTDR		REG(STM32_USART_TDR_OFFSET)
#define rBRR		REG(STM32_USART_BRR_OFFSET)
#define rCR1		REG(STM32_USART_CR1_OFFSET)
#define rCR2		REG(STM32_USART_CR2_OFFSET)
#define rCR3		REG(STM32_USART_CR3_OFFSET)
#define rGTPR		REG(STM32_USART_GTPR_OFFSET)

#define DMA_BUFFER_MASK    (ARMV7M_DCACHE_LINESIZE - 1)
#define DMA_ALIGN_UP(n)    (((n) + DMA_BUFFER_MASK) & ~DMA_BUFFER_MASK)

uint8_t ArchPX4IOSerial::_io_buffer_storage[DMA_ALIGN_UP(sizeof(IOPacket))];

ArchPX4IOSerial::ArchPX4IOSerial() :
	_tx_dma(nullptr),
	_rx_dma(nullptr),
	_current_packet(nullptr),
	_rx_dma_status(_dma_status_inactive),
	_completion_semaphore(SEM_INITIALIZER(0)),
	_pc_dmaerrs(perf_alloc(PC_COUNT, MODULE_NAME": DMA errors"))
{
}

ArchPX4IOSerial::~ArchPX4IOSerial()
{
	if (_tx_dma != nullptr) {
		stm32_dmastop(_tx_dma);
		stm32_dmafree(_tx_dma);
	}

	if (_rx_dma != nullptr) {
		stm32_dmastop(_rx_dma);
		stm32_dmafree(_rx_dma);
	}

	/* reset the UART */
	rCR1 = 0;
	rCR2 = 0;
	rCR3 = 0;

	/* detach our interrupt handler */
	up_disable_irq(PX4IO_SERIAL_VECTOR);
	irq_detach(PX4IO_SERIAL_VECTOR);

	/* restore the GPIOs */
	px4_arch_unconfiggpio(PX4IO_SERIAL_TX_GPIO);
	px4_arch_unconfiggpio(PX4IO_SERIAL_RX_GPIO);

	/* Disable APB clock for the USART peripheral */
	modifyreg32(PX4IO_SERIAL_RCC_REG, PX4IO_SERIAL_RCC_EN, 0);

	/* and kill our semaphores */
	px4_sem_destroy(&_completion_semaphore);

	perf_free(_pc_dmaerrs);
}

int
ArchPX4IOSerial::init()
{
	/* initialize base implementation */
	int r = PX4IO_serial::init((IOPacket *)&_io_buffer_storage[0]);

	if (r != 0) {
		return r;
	}

	/* allocate DMA */
	_tx_dma = stm32_dmachannel(PX4IO_SERIAL_TX_DMAMAP);
	_rx_dma = stm32_dmachannel(PX4IO_SERIAL_RX_DMAMAP);

	if ((_tx_dma == nullptr) || (_rx_dma == nullptr)) {
		return -1;
	}

	/* Enable the APB clock for the USART peripheral */
	modifyreg32(PX4IO_SERIAL_RCC_REG, 0, PX4IO_SERIAL_RCC_EN);

	/* configure pins for serial use */
	px4_arch_configgpio(PX4IO_SERIAL_TX_GPIO);
	px4_arch_configgpio(PX4IO_SERIAL_RX_GPIO);

	/* reset & configure the UART */
	rCR1 = 0;
	rCR2 = 0;
	rCR3 = 0;

	/* clear data that may be in the RDR and clear overrun error: */
	if (rISR & USART_ISR_RXNE) {
		(void)rRDR;
	}

	rICR = rISR & rISR_ERR_FLAGS_MASK;	/* clear the flags */

	/* configure line speed */
	uint32_t usartdiv32 = (PX4IO_SERIAL_CLOCK + (PX4IO_SERIAL_BITRATE) / 2) / (PX4IO_SERIAL_BITRATE);
	rBRR = usartdiv32;

	/* attach serial interrupt handler */
	irq_attach(PX4IO_SERIAL_VECTOR, _interrupt, this);
	up_enable_irq(PX4IO_SERIAL_VECTOR);

	/* enable UART in DMA mode, enable error and line idle interrupts */
	rCR3 = USART_CR3_EIE;
	/* TODO: maybe use DDRE */

	rCR1 = USART_CR1_RE | USART_CR1_TE | USART_CR1_UE | USART_CR1_IDLEIE;
	/* TODO: maybe we need to adhere to the procedure as described in the reference manual page 1251 (34.5.2) */

	/* create semaphores */
	px4_sem_init(&_completion_semaphore, 0, 0);

	/* _completion_semaphore use case is a signal */

	px4_sem_setprotocol(&_completion_semaphore, SEM_PRIO_NONE);

	/* XXX this could try talking to IO */

	return 0;
}

int
ArchPX4IOSerial::ioctl(unsigned operation, unsigned &arg)
{
	switch (operation) {

	case 1:		/* XXX magic number - test operation */
		switch (arg) {
		case 0:
			syslog(LOG_INFO, "test 0\n");

			/* kill DMA, this is a PIO test */
			stm32_dmastop(_tx_dma);
			stm32_dmastop(_rx_dma);
			rCR3 &= ~(USART_CR3_DMAR | USART_CR3_DMAT);

			for (;;) {
				while (!(rISR & USART_ISR_TXE))
					;

				rTDR = 0x55;
			}

			return 0;

		case 1: {
				unsigned fails = 0;

				for (unsigned count = 0;; count++) {
					uint16_t value = count & 0xffff;

					if (write((PX4IO_PAGE_TEST << 8) | PX4IO_P_TEST_LED, &value, 1) != 0) {
						fails++;
					}

					if (count >= 5000) {
						syslog(LOG_INFO, "==== test 1 : %u failures ====\n", fails);
						perf_print_counter(_pc_txns);
						perf_print_counter(_pc_retries);
						perf_print_counter(_pc_timeouts);
						perf_print_counter(_pc_crcerrs);
						perf_print_counter(_pc_dmaerrs);
						perf_print_counter(_pc_protoerrs);
						perf_print_counter(_pc_uerrs);
						perf_print_counter(_pc_idle);
						perf_print_counter(_pc_badidle);
						count = 0;
					}
				}

				return 0;
			}

		case 2:
			syslog(LOG_INFO, "test 2\n");
			return 0;
		}

	default:
		break;
	}

	return -1;
}

int
ArchPX4IOSerial::_bus_exchange(IOPacket *_packet)
{
	_current_packet = _packet;

	/* clear data that may be in the RDR and clear overrun error: */
	if (rISR & USART_ISR_RXNE) {
		(void)rRDR;
	}

	rICR = rISR & rISR_ERR_FLAGS_MASK;	/* clear the flags */

	/* start RX DMA */
	perf_begin(_pc_txns);

	/* DMA setup time ~3µs */
	_rx_dma_status = _dma_status_waiting;

	/*
	 * Note that we enable circular buffer mode as a workaround for
	 * there being no API to disable the DMA FIFO. We need direct mode
	 * because otherwise when the line idle interrupt fires there
	 * will be packet bytes still in the DMA FIFO, and we will assume
	 * that the idle was spurious.
	 *
	 * XXX this should be fixed with a NuttX change.
	 */
	stm32_dmasetup(
		_rx_dma,
		PX4IO_SERIAL_BASE + STM32_USART_RDR_OFFSET,
		reinterpret_cast<uint32_t>(_current_packet),
		sizeof(*_current_packet),
		DMA_SCR_CIRC		|	/* XXX see note above */
		DMA_SCR_DIR_P2M		|
		DMA_SCR_MINC		|
		DMA_SCR_PSIZE_8BITS	|
		DMA_SCR_MSIZE_8BITS	|
		DMA_SCR_PBURST_SINGLE	|
		DMA_SCR_MBURST_SINGLE);
	rCR3 |= USART_CR3_DMAR;
	stm32_dmastart(_rx_dma, _dma_callback, this, false);

	/* Clean _current_packet, so DMA can see the data */
	up_clean_dcache((uintptr_t)_current_packet,
			(uintptr_t)_current_packet + DMA_ALIGN_UP(sizeof(IOPacket)));

	/* start TX DMA - no callback if we also expect a reply */
	/* DMA setup time ~3µs */
	stm32_dmasetup(
		_tx_dma,
		PX4IO_SERIAL_BASE + STM32_USART_TDR_OFFSET,
		reinterpret_cast<uint32_t>(_current_packet),
		PKT_SIZE(*_current_packet),
		DMA_SCR_DIR_M2P		|
		DMA_SCR_MINC		|
		DMA_SCR_PSIZE_8BITS	|
		DMA_SCR_MSIZE_8BITS	|
		DMA_SCR_PBURST_SINGLE	|
		DMA_SCR_MBURST_SINGLE);
	rCR3 |= USART_CR3_DMAT;
	stm32_dmastart(_tx_dma, nullptr, nullptr, false);

	/* compute the deadline for a 10ms timeout */
	struct timespec abstime;
	clock_gettime(CLOCK_REALTIME, &abstime);
	abstime.tv_nsec += 10 * 1000 * 1000;

	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
		abstime.tv_sec++;
		abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	/* wait for the transaction to complete - 64 bytes @ 1.5Mbps ~426µs */
	int ret;
	irqstate_t irqs = enter_critical_section();

	for (;;) {
		ret = sem_timedwait(&_completion_semaphore, &abstime);

		if (ret == OK) {
			/* check for DMA errors */
			if (_rx_dma_status & DMA_STATUS_TEIF) {
				/* One of 3 things has happened:
				 *	1. a DMA Stream error
				 *	2. Serial parity, framing or over run error
				 *	3. packet is malformed
				 * In all cases DMA is stopped by either HW or the ISR error service path.
				 */
				perf_count(_pc_dmaerrs);
				ret = -EIO;
				break;

			} else {
				/* successful DMA completion but the crc can still fail */
				break;
			}

		} else {
			if (errno == ETIMEDOUT) {
				/* something has broken - clear out any partial DMA state and reconfigure */
				_abort_dma();
				_rx_dma_status = _dma_status_inactive;
				/* Wait fot at least a character time to make sure that there is no lingering
				 * IDLE interrupt triggering right after we re-enable interrupts for the next
				 * exchange
				 */
				usleep(100);
				perf_count(_pc_timeouts);
				perf_cancel(_pc_txns);		/* don't count this as a transaction */
				break;
			}
		}

		/* Loop in case we are interrupted on sleep */
	}

	_rx_dma_status = _dma_status_inactive;
	leave_critical_section(irqs);

	if (ret == OK) {
		/* check packet CRC - corrupt packet errors mean IO receive CRC error */
		uint8_t crc = _current_packet->crc;
		_current_packet->crc = 0;

		if ((crc != crc_packet(_current_packet)) || (PKT_CODE(*_current_packet) == PKT_CODE_CORRUPT)) {
			perf_count(_pc_crcerrs);
			ret = -EIO;
		}
	}

	/* update counters */
	perf_end(_pc_txns);

	return ret;
}

void
ArchPX4IOSerial::_dma_callback(DMA_HANDLE handle, uint8_t status, void *arg)
{
	if (arg != nullptr) {
		ArchPX4IOSerial *ps = reinterpret_cast<ArchPX4IOSerial *>(arg);

		ps->_do_rx_dma_callback(status);
	}
}

void
ArchPX4IOSerial::_do_rx_dma_callback(unsigned status)
{
	/* on completion of a reply, wake the waiter */
	if (_rx_dma_status == _dma_status_waiting) {

		/* check for packet overrun - this will occur after DMA completes */
		uint32_t sr = rISR;

		if (sr & (USART_ISR_ORE | USART_ISR_RXNE)) {
			(void)rRDR;
			rICR = sr & (USART_ISR_ORE | USART_ISR_RXNE);
			status = DMA_STATUS_TEIF;
		}

		/* save RX status */
		_rx_dma_status = status;

		/* disable UART DMA */
		rCR3 &= ~(USART_CR3_DMAT | USART_CR3_DMAR);
		stm32_dmastop(_tx_dma);
		stm32_dmastop(_rx_dma);

		/* complete now */
		px4_sem_post(&_completion_semaphore);
	}
}

int
ArchPX4IOSerial::_interrupt(int irq, void *context, void *arg)
{
	if (arg != nullptr) {
		ArchPX4IOSerial *instance = reinterpret_cast<ArchPX4IOSerial *>(arg);

		instance->_do_interrupt();
	}

	return 0;
}

void
ArchPX4IOSerial::_do_interrupt()
{
	uint32_t sr = rISR;	/* get UART status register */

	if (sr & USART_ISR_RXNE) {
		(void)rRDR;	/* read DR to clear RXNE */
	}

	rICR = sr & rISR_ERR_FLAGS_MASK;	/* clear flags */

	if (sr & (USART_ISR_ORE |		/* overrun error - packet was too big for DMA or DMA was too slow */
		  USART_ISR_NF |		/* noise error - we have lost a byte due to noise */
		  USART_ISR_FE)) {		/* framing error - start/stop bit lost or line break */

		/*
		 * If we are in the process of listening for something, these are all fatal;
		 * abort the DMA with an error.
		 */
		if (_rx_dma_status == _dma_status_waiting) {
			_abort_dma();

			perf_count(_pc_uerrs);
			/* complete DMA as though in error */
			_do_rx_dma_callback(DMA_STATUS_TEIF);

			return;
		}

		/* XXX we might want to use FE / line break as an out-of-band handshake ... handle it here */

		/* don't attempt to handle IDLE if it's set - things went bad */
		return;
	}

	if (sr & USART_ISR_IDLE) {

		/* if there is DMA reception going on, this is a short packet */
		if (_rx_dma_status == _dma_status_waiting) {
			/* Invalidate _current_packet, so we get fresh data from RAM */
			up_invalidate_dcache((uintptr_t)_current_packet,
					     (uintptr_t)_current_packet + DMA_ALIGN_UP(sizeof(IOPacket)));

			/* verify that the received packet is complete */
			size_t length = sizeof(*_current_packet) - stm32_dmaresidual(_rx_dma);

			if ((length < 1) || (length < PKT_SIZE(*_current_packet))) {
				perf_count(_pc_badidle);

				/* stop the receive DMA */
				stm32_dmastop(_rx_dma);

				/* error flag completion of short reception */
				_do_rx_dma_callback(DMA_STATUS_TEIF);
				return;
			}

			perf_count(_pc_idle);

			/* stop the receive DMA */
			stm32_dmastop(_rx_dma);

			/* complete the short reception */
			_do_rx_dma_callback(DMA_STATUS_TCIF);
		}
	}
}

void
ArchPX4IOSerial::_abort_dma()
{
	/* disable UART DMA */
	rCR3 &= ~(USART_CR3_DMAT | USART_CR3_DMAR);

	/* stop DMA */
	stm32_dmastop(_tx_dma);
	stm32_dmastop(_rx_dma);


	/* clear data that may be in the RDR and clear overrun error: */
	if (rISR & USART_ISR_RXNE) {
		(void)rRDR;
	}

	rICR = rISR & rISR_ERR_FLAGS_MASK;	/* clear the flags */
}
