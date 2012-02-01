#include <stdlib.h>
#include <stdio.h>

#include "databuffer.h"

#include "rangecode.h"

/*******************************************************************************
* The range coder in this file is based on the carry-less range coder          *
* by Dmitry Subbotin and the imlementation found here:                         *
* http://www.sachingarg.com/compression/entropy_coding/64bit/                  *
*******************************************************************************/

unsigned const int maxrange = 0xFFFFFFFF;
unsigned const int top = 0x01<<24;
unsigned const int bottom = 0x01<<16;


/*******************************************************************************
* This function creates a new range coder using a zero or first order model    *
*                                                                              *
* order specifies the order of the markov chain model used for prediciton      *
*                                                                              *
* Returns a new range coder struct                                             *
*******************************************************************************/
struct rangecoder *rangecoder_create( int order, int bits )
{
	struct rangecoder *coder;
	int i;
	int symbols, fsize, tsize;

	if( order < 0 )
	{
		perror( "rangecoder_create: out of order" );
		return NULL;
	}

	coder = malloc( sizeof( *coder ) );
	if( coder == NULL )
	{
		perror( "rangecoder_create: malloc" );
		return NULL;
	}

	coder->order = order;
	coder->bits = bits;
	
	symbols = 1<<bits;
	fsize = 1<<(bits*(order+1));
	tsize = 1<<(bits*order);

	coder->symbols = symbols;
	coder->fsize = fsize;
	coder->tsize = tsize;

	coder->freqs = malloc( sizeof( *coder->freqs ) * fsize );
	if( coder->freqs == NULL )
	{
		perror( "rangecoder_create: malloc" );
		return NULL;
	}

	coder->totals = malloc( sizeof( *coder->freqs ) * tsize );
	if( coder->freqs == NULL )
	{
		perror( "rangecoder_create: malloc" );
		return NULL;
	}

	for( i=0; i<fsize; i++ )
		coder->freqs[i] = 1;
	
	for( i=0; i<tsize; i++ )
		coder->totals[i] = symbols;

	return coder;
}

/*******************************************************************************
* This function resets the model of a range coder                              *
*                                                                              *
* coder is the range coder that contains the model to be reset                 *
*                                                                              *
* Modifies coder                                                               *
*******************************************************************************/
void rangecoder_reset( struct rangecoder *coder )
{
	int i;

	for( i=0; i<coder->fsize; i++ )
		coder->freqs[i] = 1;
	
	for( i=0; i<coder->tsize; i++ )
		coder->totals[i] = coder->symbols;
}

/*******************************************************************************
* This function frees a range coder and its internal structures                *
*                                                                              *
* coder is the range coder to be freed                                         *
*                                                                              *
* Modifies coder                                                               *
*******************************************************************************/
void rangecoder_free( struct rangecoder *coder )
{
	free( coder->freqs );
	free( coder->totals );
	free( coder );
}

/*******************************************************************************
* This function compresses a databuffer using a range coder                    *
*                                                                              *
* coder is the range coder to be used during compression                       *
* in contains the data to be compressed                                        *
* out is the databuffer that the compressed data will be written to            *
*                                                                              *
* Modifies coder, in and out                                                   *
*******************************************************************************/
int rangecode_compress( struct rangecoder *coder, struct databuffer *in, struct databuffer *out )
{
	int *freqs, *totals;
	unsigned int count;
	int i;
	int bits, symbol, idx, mask;
	int start, size, total;

	unsigned int low = 0x00;
	unsigned int range = maxrange;

	freqs = coder->freqs;
	totals = coder->totals;
	bits = coder->bits;

	mask = ~((~0x00)<<(bits*(coder->order+1)));

	idx = 0x00;

	for( count=0; count<in->size; count++ )
	{
		if( bits == 8 )
			symbol = databuffer_get_byte( in );
		else
			symbol = databuffer_get_bits( in, bits );

		start = 0;
		for( i=0; i<symbol; i++ )
			start += freqs[idx+i];

		size = freqs[idx+symbol];
		total = totals[idx>>bits];

		range /= total;
		low += start * range;
		range *= size;

		while( ( (low^(low+range)) < top ) || ( range < bottom ) )
		{
			if( ( range < bottom ) && ( ( (low^(low+range)) >= top ) ) )
				range = (-low)&(bottom-1);

			databuffer_add_byte( ( low >> 24 ) & 0xFF, out );
			low <<= 8;
			range <<= 8;
		}

		freqs[idx+symbol] += 32;
		totals[idx>>bits] += 32;

		if( totals[idx>>bits] >= 0xFFFF )
		{
			totals[idx>>bits] = 0;
			for( i=0; i<coder->symbols; i++ )
			{
				freqs[idx+i] /= 2;
				if( freqs[idx+i] == 0 )
					freqs[idx+i] = 1;
				totals[idx>>bits] += freqs[idx+i];
			}
		}

		idx = ((idx+symbol)<<bits)&mask;
	}

	for( i=0; i<4; i++ )
	{
		databuffer_add_byte( ( low >> 24 ) & 0xFF, out );
		low <<= 8;
	}

	return 1;
}

/*******************************************************************************
* This function decompresses a databuffer using a range coder                  *
*                                                                              *
* coder is the range coder to be used during compression                       *
* in contains the data to be decompressed                                      *
* out is the databuffer that the decompressed data will be written to          *
* length is the uncompressed data length                                       *
*                                                                              *
* Modifies coder, in and out                                                   *
*******************************************************************************/

int rangecode_decompress( struct rangecoder *coder, struct databuffer *in, struct databuffer *out, unsigned int length )
{
	int *freqs, *totals;
	unsigned int count;
	int i;
	int start, size, total, value;
	int bits, symbol, idx, mask;

	unsigned int low = 0x00;
	unsigned int range = maxrange;
	unsigned int code = 0x00;

	freqs = coder->freqs;
	totals = coder->totals;
	bits = coder->bits;

	for( i=0; i<4; i++ )
	{
		code <<= 8;
		code |= databuffer_get_byte( in ) & 0xFF;
	}

	mask = ~((~0x00)<<(bits*(coder->order+1)));

	idx = 0x00;

	for( count=0; count<length; count++ )
	{
		total = totals[idx<<bits];

		value = ( code - low ) / ( range / total );

		i = 0;
		while( ( value >= 0 ) && ( i < coder->symbols ) )
		{
			value -= freqs[idx+i];
			i++;
		}

		if( value >= 0 )
		{
			fputs( "rangecode_decompress: decompression error\n", stderr );
			return 0;
		}

		symbol = i-1;

		if( bits == 8 )
			databuffer_add_byte( symbol, out );
		else
			databuffer_add_bits( symbol, out, bits );

		start = 0;
		for( i=0; i<symbol; i++ )
			start += freqs[idx+i];

		size = freqs[idx+symbol];

		range /= total;
		low += start * range;
		range *= size;


		while( ( (low^(low+range)) < top ) || ( range < bottom ) )
		{
			if( ( range < bottom ) && ( ( (low^(low+range)) >= top ) ) )
				range = (-low)&(bottom-1);

			code <<= 8;
			code |= databuffer_get_byte( in ) & 0xFF;

			low <<= 8;
			range <<= 8;
		}

		freqs[idx+symbol] += 32;
		totals[idx>>bits] += 32;

		if( totals[idx>>bits] >= 0xFFFF )
		{
			totals[idx>>bits] = 0;
			for( i=0; i<coder->symbols; i++ )
			{
				freqs[idx+i] /= 2;
				if( freqs[idx+i] == 0 )
					freqs[idx+i] = 1;
				totals[idx>>bits] += freqs[idx+i];
			}
		}

		idx = ((idx+symbol)<<bits)&mask;
	}

	return 1;
}

