# eVY1_SMF_player
YAHAMA eVY1 standard MIDI file player

MicroSDカードからSMF（スタンダードMIDIファイル）を読み込み、データをシリアルでeVY1シールドに転送して再生します。 MIDIファイル形式は、Format 0のみ対応しています。

動作確認は、FRDM-K64Fで行っています。

eVY1を使用した場合、MIDIデータのCH.1は強制的にeVocalodによる歌声として使用されてしまうため（プログラムチェンジも不可）、強制的にCH.16に割り当てています。そのため、CH.16を使用しているMIDIファイルはデータ通りに再生する事が出来ません。
