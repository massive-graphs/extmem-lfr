#!/bin/bash

# runs
RUNS=5

# create parentfolder
mkdir -p convergence_tests

# create subfolder
now="$(date +'%d-%m-%Y')"
echo "[CONVERGENCE] Date = ${now}"
count=$(find ./convergence_tests -type d | wc -l)
echo "[CONVERGENCE] Folder Count: $((${count}-1))"
echo "[CONVERGENCE] Next Folder Index: ${count}"
foldername="log${count}_${now}"
mkdir -p convergence_tests/${foldername}
echo "[CONVERGENCE] Created Folder: convergence_tests/${foldername}"

#gamma=(2.0)
#mindeg=(10)
#nodes=(1000)
#divisor=(10)
gamma=(1.5 1.8 2.0)
mindeg=(5 10 20 100)
nodes=(10000 50000 100000 150000)
divisor=(10 200)
for g in ${gamma[*]};
do
	for a in ${mindeg[*]};
		do 
			for n in ${nodes[*]};
                do 
                    for div in ${divisor[*]};
                    do
                        for j in `seq 1 $RUNS`;
                        do
                            b=$(($n/$div))
                            echo num_nodes $n >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.log
                            echo min_deg $a >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.log
			                echo max_deg $b >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.log
                            echo gamma $g >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.log
                            echo divisor $div >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.log
                            ./build/cm_emtfp_convergence -a $a -b $b -g $g -n $n >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.log
                            mv ./graph.metis hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.graphdata
                            #python3 hh_demo.py >> hh_cm_emes_${a}_${b}_${g}_${div}_${n}_${j}.graphdata
                        done
                    done
                done
        done
done

# move files
mv hh_cm_emes_*.log convergence_tests/${foldername}
mv hh_cm_emes_*.graphdata convergence_tests/${foldername}
echo "[CONVERGENCE] Moved Log Files"