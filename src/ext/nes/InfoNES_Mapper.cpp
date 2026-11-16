/*===================================================================*/
/*                                                                   */
/*  InfoNES_Mapper.cpp : InfoNES Mapper Function                     */
/*                                                                   */
/*  2000/05/16  InfoNES Project ( based on NesterJ and pNesX )       */
/*                                                                   */
/*===================================================================*/

/*-------------------------------------------------------------------*/
/*  Include files                                                    */
/*-------------------------------------------------------------------*/

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Mapper.h"
#include "K6502.h"

/*-------------------------------------------------------------------*/
/*  Mapper resources                                                 */
/*-------------------------------------------------------------------*/

/* Disk System RAM */
BYTE *DRAM;  /* only mapper 235 used it, not built here */

/*-------------------------------------------------------------------*/
/*  Table of Mapper initialize function                              */
/*-------------------------------------------------------------------*/

struct MapperTable_tag MapperTable[] =
{
  {   0, Map0_Init },
  {   1, Map1_Init },
  {   2, Map2_Init },
  {   3, Map3_Init },
  {   4, Map4_Init },
  {   7, Map7_Init },
  {   9, Map9_Init },
  {  10, Map10_Init },
  {  11, Map11_Init },
  {  66, Map66_Init },
  {  71, Map71_Init },
  { -1, NULL }
};

/* Modified for this port: a curated set of mappers. */
#include "mapper/InfoNES_Mapper_000.inc"
#include "mapper/InfoNES_Mapper_001.inc"
#include "mapper/InfoNES_Mapper_002.inc"
#include "mapper/InfoNES_Mapper_003.inc"
#include "mapper/InfoNES_Mapper_004.inc"
#include "mapper/InfoNES_Mapper_007.inc"
#include "mapper/InfoNES_Mapper_009.inc"
#include "mapper/InfoNES_Mapper_010.inc"
#include "mapper/InfoNES_Mapper_011.inc"
#include "mapper/InfoNES_Mapper_066.inc"
#include "mapper/InfoNES_Mapper_071.inc"

/* Added for this port: mapper registers for save states (only the mappers
   built here keep state of their own; the rest is in the bank pointers). */
void InfoNES_StateMapper( void (*mem)( void *, int ) )
{
#define V( x ) mem( &x, sizeof x )
  switch ( MapperNo )
  {
    case 1:
      V( Map1_Regs ); V( Map1_Cnt ); V( Map1_Latch ); V( Map1_Last_Write_Addr ); V( Map1_Size );
      V( Map1_256K_base ); V( Map1_swap ); V( Map1_bank1 ); V( Map1_bank2 ); V( Map1_bank3 );
      V( Map1_bank4 ); V( Map1_HI1 ); V( Map1_HI2 );
      break;
    case 4:
      V( Map4_Regs ); V( Map4_Rom_Bank ); V( Map4_Prg0 ); V( Map4_Prg1 ); V( Map4_Chr01 ); V( Map4_Chr23 );
      V( Map4_Chr4 ); V( Map4_Chr5 ); V( Map4_Chr6 ); V( Map4_Chr7 ); V( Map4_IRQ_Enable ); V( Map4_IRQ_Cnt );
      V( Map4_IRQ_Latch ); V( Map4_IRQ_Request ); V( Map4_IRQ_Present ); V( Map4_IRQ_Present_Vbl );
      break;
    case 9: V( latch1 ); V( latch2 ); break;
    case 10: V( latch3 ); V( latch4 ); break;
  }
#undef V
}
