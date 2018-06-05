﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.IO;
using ASELib;
using System.Diagnostics;
using System.Threading;

namespace ExtractMAFLines
{
    class Program {
        static int nSelected = 0;
        static int nTotal = 0;
        static int nSilent = 0;

        static void WorkerThread(List<List<ASETools.Case>> workQueue, ASETools.Configuration configuration)
        {
            while (true)
            {
                List<ASETools.Case> casesForThisMAF;

                lock (workQueue)
                {
                    if (workQueue.Count() == 0)
                    {
                        return;
                    }

                    casesForThisMAF = workQueue[0];
                    workQueue.RemoveAt(0);
                }

                if (configuration.isBeatAML && casesForThisMAF.Count() != 1)
                {
                    Console.WriteLine("BeatAML MAFs must be one/case.");
                    continue;
                }

                    var timer = new Stopwatch();
                timer.Start();

                var mafFilename = casesForThisMAF[0].maf_filename;

                var mafLines = ASETools.MAFLine.ReadFile(casesForThisMAF[0].maf_filename, casesForThisMAF[0].maf_file_id, true, configuration.isBeatAML);

                int nSelectedThisDisease = 0;
                int nTotalThisDisease = 0;
                int nSilentThisDisease = 0;

                foreach (var case_ in casesForThisMAF)
                {
                    string caseDirectory;

                    if (configuration.isBeatAML)
                    {
                        caseDirectory = configuration.dataDirectories[0] + @"..\" + configuration.derivedFilesDirectory + @"\" + case_.case_id + @"\";
                    }
                    else
                    {
                        caseDirectory  = ASETools.GetDirectoryFromPathname(mafFilename) + @"\..\..\" + configuration.derivedFilesDirectory + @"\" + case_.case_id + @"\";
                    }

                    Directory.CreateDirectory(caseDirectory);  // This is a no-op if the directory already exists.

                    if (doExtractedMafLineFile && case_.extracted_maf_lines_filename == "")
                    {
                        var selectedLines = mafLines.Where(x => (configuration.isBeatAML ||  case_.tumor_dna_file_id == x.tumor_bam_uuid) &&    // BeatAML doesn't have the uuid fields, but it's one MAF/case, so you can tell from that.
                        !(x.n_alt_count >= 10 ||
                          x.t_depth == 0 ||
                          x.n_depth > 0 && (double)x.n_alt_count / (double)x.n_depth * 5.0 >= (double)x.t_alt_count / (double)x.t_depth ||
                          x.Variant_Classification == "3'Flank" ||
                          x.Variant_Classification == "5'Flank" ||
                          x.Variant_Classification == "IGR" ||
                          x.Variant_Classification == "Intron" ||
                          // we now keep silent mutations to use as a control.  x.Variant_Classification == "Silent" ||
                          x.Variant_Classification == "3'UTR" ||
                          x.Variant_Classification == "5'UTR" ||
                          x.t_alt_count * 5 < x.t_depth ||
                          x.Chromosome.StartsWith("chrM") ||
                          x.Chromosome == "MT"  // The BeatAML version
                          )
                        ).ToList();    // The second half of the condition rejects MAF lines that look like germline variants (or pseudogenes that are mismapped), or are in uninteresting regions (IGRs, Introns, etc.) and minor subclones (< 20%) and mitochondrial genes

                        nSelectedThisDisease += selectedLines.Count();
                        nTotalThisDisease += mafLines.Count();
                        nSilentThisDisease += selectedLines.Where(x => x.Variant_Classification == "Silent").Count();

                        if (selectedLines.Count() == 0)
                        {
                            Console.WriteLine("Found no MAF lines for case " + case_.case_id);
                            // A few BeatAML ones have them, just generate an empty MAF.  continue;
                        }

                        ASETools.MAFLine.WriteToFile(caseDirectory + case_.case_id + ASETools.extractedMAFLinesExtension, selectedLines);
                    }

                    if (doAllMafLineFile && case_.all_maf_lines_filename == "")
                    {
                        ASETools.MAFLine.WriteToFile(caseDirectory + case_.case_id + ASETools.allMAFLinesExtension, mafLines.Where(x => case_.tumor_dna_file_id == x.tumor_bam_uuid).ToList());
                    }
                }

                lock (workQueue)
                {
                    nTotal += nTotalThisDisease;
                    nSelected += nSelectedThisDisease;
                    nSilent += nSilentThisDisease;
                }
                Console.WriteLine("selected " + nSelectedThisDisease + " (of which " + nSilentThisDisease + " were silent) of " + mafLines.Count() + " for " + mafFilename + " in " + ASETools.ElapsedTimeInSeconds(timer));
            }
        }

        static bool doAllMafLineFile = true;
        static bool doExtractedMafLineFile = true;

        static void Main(string[] args)
        {
            var stopwatch = new Stopwatch();
            stopwatch.Start();

            var configuration = ASETools.Configuration.loadFromFile(args);
            
            if (configuration.commandLineArgs.Count() >1 || 
                    configuration.commandLineArgs.Count() == 1 && configuration.commandLineArgs[0] != "-a" && configuration.commandLineArgs[0] != "-e") {
                Console.WriteLine("usage: ExtractMAFLines {-configuration configurationFileName} {-a|-e}");
                Console.WriteLine("-a means only to create the all maf line file, while -e means only to create the extracted maf line file.");
                return;
            }

            if (configuration.commandLineArgs.Count() == 1)
            {
                if (configuration.commandLineArgs[0] == "-e")
                {
                    doAllMafLineFile = false;
                } else
                {
                    doExtractedMafLineFile = false;
                }
            }
     
            var cases = ASETools.Case.LoadCases(configuration.casesFilePathname);
            
            if (null == cases)
            {
                Console.WriteLine("Unable to load cases.  You must generate it before running this tool.");
                return;
            }

            var casesByMAF = new Dictionary<string, List<ASETools.Case>>();

            int nCasesToProcess = 0;

            foreach (var caseEntry in cases)
            {
                var case_ = caseEntry.Value;

                if (case_.maf_filename == "")
                {
                    continue;
                }

                if ((case_.extracted_maf_lines_filename != "" || !doExtractedMafLineFile) && (case_.all_maf_lines_filename != "" || !doAllMafLineFile))
                {
                    // Nothing to do for this file.
                    continue;
                }

                if (!casesByMAF.ContainsKey(case_.maf_filename))
                {
                    casesByMAF.Add(case_.maf_filename, new List<ASETools.Case>());
                }

                casesByMAF[case_.maf_filename].Add(case_);
                nCasesToProcess++;
            }

            Console.WriteLine("Processing " + nCasesToProcess + " cases with " + casesByMAF.Count() + " MAF files.");


            var workQueue = new List<List<ASETools.Case>>();

            foreach (var casesForThisMAFEntry in casesByMAF)
            {
                workQueue.Add(casesForThisMAFEntry.Value);
            } // foereach MAF

            int nThreads = Math.Min(Environment.ProcessorCount, (int)(ASETools.GetTotalComputerMemory() / ((ulong)3 * 1024 * 1024 * 1024)));

            var threads = new List<Thread>();
            for (int i = 0; i < nThreads; i++)
            {
                threads.Add(new Thread(() => WorkerThread(workQueue, configuration)));
            }

            threads.ForEach(t => t.Start());
            threads.ForEach(t => t.Join());

            Console.WriteLine("Processed " + nCasesToProcess + " cases in " + casesByMAF.Count() + " MAFs, selecting " + nSelected + " (of which " + nSilent + " were silent) of " + nTotal + " in " + ASETools.ElapsedTimeInSeconds(stopwatch));
        }
    }
}
