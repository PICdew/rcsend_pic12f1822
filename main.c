/*
 * File:   main.c
 * Author: zetsutenman
 *
 * Created on 2018/12/22, 9:22
 */


#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#pragma config FOSC = INTOSC    // Oscillator Selection (INTOSC oscillator: I/O function on CLKIN pin)
#pragma config WDTE = OFF       // Watchdog Timer Enable (WDT disabled)
#pragma config PWRTE = ON       // Power-up Timer Enable (PWRT enabled)
#pragma config MCLRE = OFF      // MCLR Pin Function Select (MCLR/VPP pin function is MCLR)
#pragma config CP = OFF         // Flash Program Memory Code Protection (Program memory code protection is disabled)
#pragma config CPD = OFF        // Data Memory Code Protection (Data memory code protection is disabled)
#pragma config BOREN = OFF      // Brown-out Reset Enable (Brown-out Reset disabled)
#pragma config CLKOUTEN = OFF   // Clock Out Enable (CLKOUT function is disabled. I/O or oscillator function on the CLKOUT pin)
#pragma config IESO = OFF       // Internal/External Switchover (Internal/External Switchover mode is disabled)
#pragma config FCMEN = OFF      // Fail-Safe Clock Monitor Enable (Fail-Safe Clock Monitor is disabled)

#pragma config WRT = OFF        // Flash Memory Self-Write Protection (Write protection off)
#pragma config PLLEN = ON       // PLL Enable (4x PLL enabled)
#pragma config STVREN = ON      // Stack Overflow/Underflow Reset Enable (Stack Overflow or Underflow will cause a Reset)
#pragma config BORV = HI        // Brown-out Reset Voltage Selection (Brown-out Reset Voltage (Vbor), high trip point selected.)
#pragma config LVP = OFF        // Low-Voltage Programming Enable (High-voltage on MCLR/VPP must be used for programming)

#define _XTAL_FREQ          32000000

unsigned int count = 0;

// 設定したPWM周期毎にカウントアップ
void interrupt interCountPWMperiod() {
    if(TMR2IF == 1) {
        count++;
        TMR2IF = 0;
    }
}

/*
 * RA3のスイッチ入力を読み取り、RA2からリモコン信号を出力する
 */
void main() {

    // PIC設定
    OSCCON = 0b01110000;    // 内部クロック8MHz（PLLEN=ONなので32MHzで動作）
    ANSELA = 0b00000000;    // すべてデジタルI/Oに割当
    TRISA  = 0b00001000;    // RA3は入力、それ以外はすべて出力
    PORTA  = 0b00000000;    // すべてのピンの出力をLowとする
    WPUA   = 0b00000000;    // 内部プルアップ抵抗は無効
    
    CCP1SEL = 0;            // RA2をCCP1ピンとして出力
    CCP1CON = 0b00001100;   // PWM機能を使用する
    T2CON   = 0b00000000;   // TMR2プリスケーラ値を1倍に設定
    CCPR1L  = 0;            // デューティ比を0で初期化
    CCPR1H  = 0;
    TMR2    = 0;            // Timer2カウンターを初期化
    PR2     = 210;          // PWM周期を約38kHzで設定
    TMR2IF  = 0;            // Timer2割込フラグ初期化
    
    TMR2IE = 1;             // Timer2割込を許可
    PEIE   = 1;             // 周辺装置の割込を許可
    GIE    = 1;             // 全割込を許可
    
    long code = 0x02FD48B7;         // デバイス固有コード。カスタマーコード16bit+データコード16bit（REGZAの電源ボタン）
    signed char bit_index = 31;     // 固有コードのどのビットを見ているか
    unsigned char bit_num = 0;      // 固有コードから取り出した各ビットの値
    unsigned int count_sum = 0;     // リーダーコードorリピートコード送信開始～ストップビットON期間終了までにかかった周期カウント
    unsigned int wait_108ms = 0;    // 108ms周期にするために必要なストップビットOFF期間
    bool flag_onoff = true;
    bool flag_bit_read = true;
    bool flag_stopbit = true;
    
    // NECフォーマットに準拠
    // CCPR1L=70とするとデューティ比1/3で約38kHzのサブキャリアを生成する
    // PWM周期P=26.375μsec  
    // 変調単位T=562μsec=21.3P
    // リーダーコード　ON→16T=341P OFF→8T=171P
    // デバイス固有コード　0：ON→1T=22P、OFF→1T=20P　1：ON→1T=22P、OFF→3T=64P
    // フレーム送信周期　108ms=108000μsec=4095P
    // リピートコード　ON→16T=341P OFF→4T=85P
    
    while(true) {
        // リモコンスイッチ監視
        while(true) {
            if(RA3 == 0) { // スイッチを押したときRA3がLowになる
                CCPR1L = 70; // デューティ比を1/3に設定（210*(1/3)=70）
                TMR2ON = 1; // Timer2の動作開始
                break; // リーダーコード送信に移行
            }
        }
        
        // リーダーコード送信
        while(true) {
            if(flag_onoff && count > 340) { // リーダーコードON期間経過
                flag_onoff = false;
                CCPR1L = 0; // デューティ比0
                count_sum = count_sum + count;
                count = 0;
            }else if(!flag_onoff && count > 170) { // リーダーコードOFF期間経過
                flag_onoff = true;
                CCPR1L = 70; // デューティ比1/3
                count_sum = count_sum + count;
                count = 0;
                break; // デバイス固有コード送信に移行
            }
        }
        
        // デバイス固有コード送信
        while(true) {
            if(flag_bit_read) { // 送信するコードの各ビットの値を取り出す
                if(bit_index >= 0) {
                    bit_num = (unsigned char)((code >> bit_index) & 1); // 右シフト演算とAND演算で固有コードの最上位ビットから最下位ビットに向かって順番に値を取り出す
                    bit_index--;
                    flag_bit_read = false;
                }else{ // デバイス固有コード送信完了
                    break; // リピートコードorストップビット送信に移行
                }
            }else{
                // ビットの値によってOFFの期間を変える（1:3T=64P、0:1T=21P、コードONは22Pとして変調単位のバランスをとる）
                if(flag_onoff && count > 21) { // コードON期間経過
                    flag_onoff = false;
                    CCPR1L = 0; // デューティ比0
                    count_sum = count_sum + count;
                    count = 0;
                }else if(!flag_onoff && bit_num == 0 && count > 20) { // コードOFF期間経過
                    flag_onoff = true;
                    flag_bit_read = true; // 次のビット値取り出し
                    CCPR1L = 70; // デューティ比1/3
                    count_sum = count_sum + count;
                    count = 0;
                }else if(!flag_onoff && bit_num == 1 && count > 63) { // コードOFF期間経過
                    flag_onoff = true;
                    flag_bit_read = true; // 次のビット値取り出し
                    CCPR1L = 70; // デューティ比1/3
                    count_sum = count_sum + count;
                    count = 0;
                }
            }
        }
        
        // リピートコードorストップビット送信
        while(true) {
            if(!flag_stopbit) { // リピートコード送信
                if(flag_onoff && count > 340) { // リピートコードON期間経過
                    flag_onoff = false;
                    CCPR1L = 0; // デューティ比0
                    count_sum = count_sum + count;
                    count = 0;
                }else if(!flag_onoff && count > 85) { // リピートコードOFF期間経過
                    flag_onoff = true;
                    CCPR1L = 70; // デューティ比1/3
                    count_sum = count_sum + count;
                    count = 0;
                    flag_stopbit = true; // ストップビット送信に移行
                }
            }else{ // ストップビット送信
                if(flag_onoff && count > 21) { // ストップビットON期間経過
                    flag_onoff = false;
                    CCPR1L = 0; // デューティ比0
                    count_sum = count_sum + count;
                    wait_108ms = 4095 - count_sum; // 108ms周期にするために必要なストップビットOFF期間の計算
                    count_sum = 0;
                    count = 0;
                }else if(!flag_onoff && count > wait_108ms) { // ストップビットOFF期間経過（フレーム送信周期108msになるまでOFF）
                    flag_onoff = true;
                    CCPR1L = 70; // デューティ比1/3
                    count = 0;
                    flag_stopbit = false; // リピートコード送信に移行
                    
                    // ストップビット送信完了時点でリモコンスイッチがOFFの場合、コード送信終了
                    if(RA3 == 1) {
                        break;
                    }
                }
            }
        }
        
        // 変数を再初期化
        flag_onoff = true;
        flag_bit_read = true;
        flag_stopbit = true;
        bit_index = 31;
        TMR2ON = 0;
        TMR2 = 0;
        count = 0;
    }
    
    return;
    
}